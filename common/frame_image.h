#pragma once
// frame_image.h — minimal RGB image container + geometry + resize/chip ops.
//
// Replaces dlib's matrix/rectangle/pixel container types (v1.6: every dlib ML
// feature was already removed — 68-point, HOG, ResNet, FFT, BLAS — only these
// containers and two image functions were still in use). Removing dlib drops
// its whole vcpkg chain (blas/lapack/libjpeg/libpng/fftw3/sqlite3) from the
// build and ~14.5 MB of BLAS DLLs from the installer.
//
// PERFORMANCE CONTRACT: the resize and chip-extraction algorithms below are
// faithful ports of dlib's resize_image()/extract_image_chip() with bilinear
// interpolation and its pyramid_down<2> (5-tap [1,4,6,4,1], 2:1). Pixel output
// matches the dlib pipeline (identical double-precision bilinear blend and
// truncating uchar conversion), so detector/recognizer/PAD results — and the
// calibrated anti_spoof_threshold — are unchanged. Both paths run per PAD
// frame in the auth loop; keep them as simple loops with hoisted increments.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <vector>
#if defined(_MSC_VER) && defined(_M_X64)
#include <emmintrin.h>   // SSE2 for the 4-wide ResizeBilinear bulk path
#endif

namespace facelogin {

// 24-bit RGB pixel, same field names/layout as dlib::rgb_pixel.
struct RgbPixel {
    uint8_t red = 0, green = 0, blue = 0;

    constexpr RgbPixel() = default;
    constexpr RgbPixel(uint8_t r, uint8_t g, uint8_t b) : red(r), green(g), blue(b) {}
};
static_assert(sizeof(RgbPixel) == 3, "RgbPixel must be tightly packed (like dlib::rgb_pixel)");

// Integer rectangle, dlib::rectangle convention: right/bottom INCLUSIVE,
// default-constructed is empty (left > right). Member-FUNCTION API mirrors
// dlib::rectangle (left()/top()/right()/bottom()/width()/height()/is_empty())
// so call sites compile unchanged apart from the type rename.
struct FaceRect {
    long m_left = 0, m_top = 0, m_right = -1, m_bottom = -1;

    constexpr FaceRect() = default;
    constexpr FaceRect(long l, long t, long r, long b)
        : m_left(l), m_top(t), m_right(r), m_bottom(b) {}

    long left() const { return m_left; }
    long top() const { return m_top; }
    long right() const { return m_right; }
    long bottom() const { return m_bottom; }
    long width() const { return m_right - m_left + 1; }
    long height() const { return m_bottom - m_top + 1; }
    bool is_empty() const { return m_left > m_right || m_top > m_bottom; }
};

// Row-major RGB image (contiguous, 3 bytes/pixel) — replaces
// dlib::matrix<dlib::rgb_pixel>. Memory layout is identical, so raw row
// pointers and the (y, x) accessor behave exactly like dlib's matrix.
// Member-function API mirrors dlib (nr()/nc()/size()/set_size()) so call
// sites compile unchanged apart from the type rename.
class FrameImage {
public:
    FrameImage() = default;
    FrameImage(int rows, int cols) { set_size(rows, cols); }

    void set_size(int rows, int cols) {
        m_h = rows;
        m_w = cols;
        m_data.assign(static_cast<size_t>(rows) * cols, RgbPixel());
    }
    void clear() { m_w = m_h = 0; m_data.clear(); }
    bool is_empty() const { return m_data.empty(); }
    size_t size() const { return m_data.size(); }
    long nr() const { return m_h; }   // rows
    long nc() const { return m_w; }   // cols

    RgbPixel& operator()(long row, long col) {
        return m_data[static_cast<size_t>(row) * m_w + col];
    }
    const RgbPixel& operator()(long row, long col) const {
        return m_data[static_cast<size_t>(row) * m_w + col];
    }
    RgbPixel& operator[](size_t i) { return m_data[i]; }
    const RgbPixel& operator[](size_t i) const { return m_data[i]; }
    // dlib-compatible single-argument linear index (matrix(i)).
    RgbPixel& operator()(long i) { return m_data[static_cast<size_t>(i)]; }
    const RgbPixel& operator()(long i) const { return m_data[static_cast<size_t>(i)]; }

    // Raw bytes (R,G,B interleaved) for conversion loops / BGRA packing.
    uint8_t* raw() { return reinterpret_cast<uint8_t*>(m_data.data()); }
    const uint8_t* raw() const { return reinterpret_cast<const uint8_t*>(m_data.data()); }

private:
    long m_w = 0, m_h = 0;
    std::vector<RgbPixel> m_data;
};

/// Rotate a frame clockwise by 0, 90, 180, or 270 degrees.
/// Other values are silently treated as 0 (no-op).
/// Frame dimensions swap accordingly for 90/270.
inline void RotateFrame(FrameImage& frame, int rotation) {
    if (rotation == 0) return;

    const long h = frame.nr();
    const long w = frame.nc();

    if (rotation == 180) {
        FrameImage out(h, w);
        for (long y = 0; y < h; ++y)
            for (long x = 0; x < w; ++x)
                out(y, x) = frame(h - 1 - y, w - 1 - x);
        frame = std::move(out);
    } else if (rotation == 90) {
        FrameImage out(w, h);
        for (long y = 0; y < w; ++y)
            for (long x = 0; x < h; ++x)
                out(y, x) = frame(h - 1 - x, y);
        frame = std::move(out);
    } else if (rotation == 270) {
        FrameImage out(w, h);
        for (long y = 0; y < w; ++y)
            for (long x = 0; x < h; ++x)
                out(y, x) = frame(x, w - 1 - y);
        frame = std::move(out);
    }
}

// dlib-exact bilinear blend: double precision, truncated uchar result
// (dlib's vector_to_pixel for rgb_pixel truncates — no rounding).
// Used by ExtractChip, whose dlib reference path is also scalar double.
inline RgbPixel BlendBilinear(const RgbPixel& tl, const RgbPixel& tr,
                              const RgbPixel& bl, const RgbPixel& br,
                              double lr, double tb) {
    auto b = [&](double c00, double c10, double c01, double c11) {
        const double top = c00 + (c10 - c00) * lr;
        const double bot = c01 + (c11 - c01) * lr;
        return static_cast<uint8_t>(top + (bot - top) * tb);
    };
    return RgbPixel(b(tl.red,   tr.red,   bl.red,   br.red),
                    b(tl.green, tr.green, bl.green, br.green),
                    b(tl.blue,  tr.blue,  bl.blue,  br.blue));
}

// Float-precision blend — matches dlib's resize_image SIMD bulk path (float
// weights, truncated uchar). Used by ResizeBilinear for the 512×512 detector
// preprocess (the one hot path where dlib was SIMD); stays within ±1 LSB of
// dlib's scalar-tail results.
inline RgbPixel BlendBilinearF(const RgbPixel& tl, const RgbPixel& tr,
                               const RgbPixel& bl, const RgbPixel& br,
                               float lr, float tb) {
    auto b = [&](float c00, float c10, float c01, float c11) {
        const float top = c00 + (c10 - c00) * lr;
        const float bot = c01 + (c11 - c01) * lr;
        return static_cast<uint8_t>(top + (bot - top) * tb);
    };
    return RgbPixel(b(tl.red,   tr.red,   bl.red,   br.red),
                    b(tl.green, tr.green, bl.green, br.green),
                    b(tl.blue,  tr.blue,  bl.blue,  br.blue));
}

/// Bilinear resize into a pre-sized dst — port of dlib::resize_image:
/// x_scale=(src.w-1)/(dst.w-1), corner-anchored sampling. The 4-pixel bulk is
/// SSE2-vectorized EXACTLY like dlib's simd4f path (float coordinates and
/// weights, truncated uchar output; unclamped right-neighbor loads guaranteed
/// in-bounds by the loop break), so the 512×512 detector preprocess keeps
/// dlib-class speed and identical numerics. Scalar float tail (dlib's tail is
/// double — ±1 LSB, same as dlib's own SIMD-vs-tail split).
inline void ResizeBilinear(const FrameImage& src, FrameImage& dst) {
    const long sw = src.nc(), sh = src.nr();
    const long dw = dst.nc(), dh = dst.nr();
    if (dst.is_empty() || src.is_empty()) return;

    const float x_scale = (sw - 1.0f) / static_cast<float>(std::max(dw - 1, 1L));
    const float y_scale = (sh - 1.0f) / static_cast<float>(std::max(dh - 1, 1L));
    float y = -y_scale;
    for (long r = 0; r < dh; ++r) {
        y += y_scale;
        const long top = static_cast<long>(std::floor(y));
        const long bottom = std::min(top + 1, sh - 1);
        const float tb_frac = y - top;

        // 4-wide SSE2 bulk. _x holds 4 source x positions (float, like dlib);
        // left = trunc(x) (x ≥ 0, so trunc == floor), frac = x - left.
        const __m128 one = _mm_set1_ps(1.0f);
        const __m128 _x_scale = _mm_set1_ps(4.0f * x_scale);
        const __m128 _tb = _mm_set1_ps(tb_frac);
        const __m128 _itb = _mm_set1_ps(1.0f - tb_frac);
        // dlib starts at x = -4*x_scale and adds 4*x_scale per iteration, so
        // iteration 1 samples output cols 0-3 at source 0..3*x_scale.
        __m128 _x = _mm_set_ps(-1.0f * x_scale, -2.0f * x_scale,
                               -3.0f * x_scale, -4.0f * x_scale);
        long c = 0;
        for (;; c += 4) {
            _x = _mm_add_ps(_x, _x_scale);
            const __m128i lefti = _mm_cvttps_epi32(_x);       // truncate
            const __m128 lr = _mm_sub_ps(_x, _mm_cvtepi32_ps(lefti));
            const __m128i righti = _mm_add_epi32(lefti, _mm_set1_epi32(1));
            const __m128 ilr = _mm_sub_ps(one, lr);
            int32_t fleft[4], fright[4];
            _mm_storeu_si128(reinterpret_cast<__m128i*>(fleft), lefti);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(fright), righti);
            if (fright[3] >= sw) break;   // all 4 right-neighbors in-bounds

            // Per-channel bilinear blend, 4 lanes, dlib's formula.
            auto lane = [&](uint8_t RgbPixel::*ch, RgbPixel* outRow) {
                __m128 tl = _mm_set_ps(src(top, fleft[3]).*ch, src(top, fleft[2]).*ch,
                                       src(top, fleft[1]).*ch, src(top, fleft[0]).*ch);
                __m128 tr = _mm_set_ps(src(top, fright[3]).*ch, src(top, fright[2]).*ch,
                                       src(top, fright[1]).*ch, src(top, fright[0]).*ch);
                __m128 bl = _mm_set_ps(src(bottom, fleft[3]).*ch, src(bottom, fleft[2]).*ch,
                                       src(bottom, fleft[1]).*ch, src(bottom, fleft[0]).*ch);
                __m128 br = _mm_set_ps(src(bottom, fright[3]).*ch, src(bottom, fright[2]).*ch,
                                       src(bottom, fright[1]).*ch, src(bottom, fright[0]).*ch);
                __m128 v = _mm_add_ps(_mm_mul_ps(_itb,
                                       _mm_add_ps(_mm_mul_ps(ilr, tl), _mm_mul_ps(lr, tr))),
                                       _mm_mul_ps(_tb, _mm_add_ps(_mm_mul_ps(ilr, bl), _mm_mul_ps(lr, br))));
                __m128i outi = _mm_cvttps_epi32(v);           // truncate, like dlib
                int32_t fout[4];
                _mm_storeu_si128(reinterpret_cast<__m128i*>(fout), outi);
                for (int k = 0; k < 4; ++k)
                    outRow[k].*ch = static_cast<uint8_t>(fout[k]);
            };
            RgbPixel* outRow = &dst(r, c);   // outRow[k] == dst(r, c + k)
            lane(&RgbPixel::red, outRow);
            lane(&RgbPixel::green, outRow);
            lane(&RgbPixel::blue, outRow);
        }
        // Scalar tail (dlib re-derives x from c: x = -x_scale + c*x_scale).
        float xt = -x_scale + static_cast<float>(c) * x_scale;
        for (; c < dw; ++c) {
            xt += x_scale;
            const long left = static_cast<long>(std::floor(xt));
            const long right = std::min(left + 1, sw - 1);
            const float lr_frac = xt - left;
            dst(r, c) = BlendBilinearF(src(top, left), src(top, right),
                                       src(bottom, left), src(bottom, right),
                                       lr_frac, tb_frac);
        }
    }
}

// 2:1 Gaussian-ish downsample — port of dlib::pyramid_down<2> (RGB path):
// 5-tap [1,4,6,4,1] row filter (step 2), same column filter, /256 total.
// Output dims ((nr-3)/2, (nc-3)/2); EMPTY when either dim <= 8 (dlib clears
// the level in that case, which makes the chip sample as all-black).
inline void PyramidDown2(const FrameImage& in, FrameImage& out) {
    const long nr = in.nr(), nc = in.nc();
    if (nr <= 8 || nc <= 8) {
        out.clear();
        return;
    }
    const long orows = (nr - 3) / 2, ocols = (nc - 3) / 2;

    // Row filter into a uint16 temp (max 255*16 = 4080 fits).
    std::vector<uint16_t> temp(static_cast<size_t>(nr) * ocols * 3);
    for (long r = 0; r < nr; ++r) {
        long oc = 0;
        for (long c = 0; c < ocols; ++c) {
            const RgbPixel* p = &in(r, oc);
            uint16_t* t = &temp[(static_cast<size_t>(r) * ocols + c) * 3];
            t[0] = static_cast<uint16_t>(p[0].red   + 4 * p[1].red   + 6 * p[2].red   + 4 * p[3].red   + p[4].red);
            t[1] = static_cast<uint16_t>(p[0].green + 4 * p[1].green + 6 * p[2].green + 4 * p[3].green + p[4].green);
            t[2] = static_cast<uint16_t>(p[0].blue  + 4 * p[1].blue  + 6 * p[2].blue  + 4 * p[3].blue  + p[4].blue);
            oc += 2;
        }
    }

    out.set_size(orows, ocols);
    long dr = 0;
    for (long r = 2; r < nr - 2; r += 2) {
        const uint16_t* t0 = &temp[(static_cast<size_t>(r - 2) * ocols) * 3];
        const uint16_t* t1 = t0 + 3 * ocols;
        const uint16_t* t2 = t1 + 3 * ocols;
        const uint16_t* t3 = t2 + 3 * ocols;
        const uint16_t* t4 = t3 + 3 * ocols;
        uint8_t* o = out.raw() + (static_cast<size_t>(dr) * ocols) * 3;
        for (long c = 0; c < ocols; ++c) {
            o[0] = static_cast<uint8_t>((t0[0] + 4 * t1[0] + 6 * t2[0] + 4 * t3[0] + t4[0]) / 256);
            o[1] = static_cast<uint8_t>((t0[1] + 4 * t1[1] + 6 * t2[1] + 4 * t3[1] + t4[1]) / 256);
            o[2] = static_cast<uint8_t>((t0[2] + 4 * t1[2] + 6 * t2[2] + 4 * t3[2] + t4[2]) / 256);
            t0 += 3; t1 += 3; t2 += 3; t3 += 3; t4 += 3;
            o += 3;
        }
        ++dr;
    }
}

// Affine chip extraction — exact port of dlib's
//   extract_image_chip(img, chip_details(rect, chip_dims(rows, cols)),
//                      chip, interpolate_bilinear())
// for the axis-aligned (angle==0) case this project always uses. `rect` is an
// inclusive-corner rectangle; `out` is resized to rows×cols. Large
// downsampling goes through the same Gaussian pyramid as dlib (levels built
// from a border-grown bounding box), so the MiniFAS crop — and therefore PAD
// scores — match the previous pipeline pixel-for-pixel. Out-of-range samples
// are black.
inline void ExtractChip(const FrameImage& src, const FaceRect& rect,
                        int rows, int cols, FrameImage& out) {
    const long sw = src.nc(), sh = src.nr();
    out.set_size(rows, cols);
    if (sw <= 0 || sh <= 0 || rows < 2 || cols < 2 || rect.is_empty()) {
        return;   // all-black (out was zeroed by set_size) — dlib behaves the same
    }

    // --- pyramid depth + bounding box (dlib extract_image_chips) ----------
    // drectangle in dlib = double corners; point_down(p) = p/2 - (1.25, 0.75).
    auto pointDown = [](double& px, double& py) {
        px = px / 2.0 - 1.25;
        py = py / 2.0 - 0.75;
    };
    auto rectDownArea = [&](double l, double t, double r, double b) {
        pointDown(l, t); pointDown(r, b);
        return (r - l) * (b - t);
    };

    const double chipArea = static_cast<double>(rows) * cols;
    long depth = 0;
    double grow = 2.0;
    // dlib keeps TWO coordinate chains: the down-sampled `rect` is used ONLY
    // to count pyramid depth, while the bounding box is grown from the
    // ORIGINAL rect (rot_rect). Mixing them shrinks the box by 2^depth and
    // feeds the chip extraction garbage — PAD scores collapse.
    const double rectL = rect.left(), rectT = rect.top();
    const double rectR = rect.right(), rectB = rect.bottom();
    double rl = rectL, rt = rectT, rr = rectR, rb = rectB;
    while (rectDownArea(rl, rt, rr, rb) > chipArea) {
        pointDown(rl, rt); pointDown(rr, rb);   // rect = rect_down(rect)
        ++depth;
        grow = grow * 2.0 + 2.0;
    }

    // bounding_box = grow_rect(ORIGINAL rect, grow) ∩ image rect (integers
    // here, so the drectangle→rectangle conversion is exact).
    long bl = static_cast<long>(rectL - grow), bt = static_cast<long>(rectT - grow);
    long br = static_cast<long>(rectR + grow), bb = static_cast<long>(rectB + grow);
    bl = std::max(bl, 0L); bt = std::max(bt, 0L);
    br = std::min(br, sw - 1);
    bb = std::min(bb, sh - 1);

    // --- build the pyramid levels (levels[0] = pyr(bbox crop), ...) --------
    // Empty levels (bbox ≤ 8×8) produce all-black chips via the sampling
    // range check below, matching dlib's transform_image on an empty level.
    std::vector<FrameImage> levels(static_cast<size_t>(depth));
    if (depth > 0 && bl <= br && bt <= bb) {
        FrameImage crop((bb - bt + 1), (br - bl + 1));
        for (long y = 0; y < crop.nr(); ++y) {
            const RgbPixel* s = &src(bt + y, bl);
            RgbPixel* d = &crop(y, 0);
            std::copy(s, s + crop.nc(), d);
        }
        PyramidDown2(crop, levels[0]);
        for (size_t k = 1; k < levels.size(); ++k)
            PyramidDown2(levels[k - 1], levels[k]);
    }

    // --- per-chip level selection + affine mapping (angle == 0) ------------
    // rect2 = translate(rect, -bbox.tl), downed until rect_down(rect2).area()
    // ≤ chip area; level == depth-1 always for our single chip.
    double l2 = static_cast<double>(rect.left()) - bl;
    double t2 = static_cast<double>(rect.top()) - bt;
    double r2 = static_cast<double>(rect.right()) - bl;
    double b2 = static_cast<double>(rect.bottom()) - bt;
    int level = -1;
    while (rectDownArea(l2, t2, r2, b2) > chipArea) {
        pointDown(l2, t2); pointDown(r2, b2);
        ++level;
    }

    // Source image for sampling: level == -1 → bbox crop of src (accessed via
    // src + bbox origin); otherwise the pyramid level (own coordinates).
    const FrameImage* levelImg = nullptr;
    long srcBaseX = 0, srcBaseY = 0;
    if (level == -1) {
        srcBaseX = bl;
        srcBaseY = bt;
    } else if (!levels.empty()) {
        levelImg = &levels[static_cast<size_t>(level)];
    } else {
        return;   // depth was 0 but the loop ran? unreachable — guard.
    }

    // Chip pixel (c, r) → source: corner-anchored affine from the chip's tl/tr
    // corners onto rect2's tl/tr/br (dlib find_affine_transform for these
    // non-collinear points). Axis-aligned rect2 keeps it separable.
    const double stepX = (r2 - l2) / static_cast<double>(cols - 1);
    const double stepY = (b2 - t2) / static_cast<double>(rows - 1);

    for (int r = 0; r < rows; ++r) {
        const double sy = t2 + r * stepY;
        const long top = static_cast<long>(std::floor(sy));
        const double tb_frac = sy - top;
        for (int c = 0; c < cols; ++c) {
            const double sx = l2 + c * stepX;
            const long left = static_cast<long>(std::floor(sx));
            const double lr_frac = sx - left;

            // dlib interpolate_bilinear strict in-range rule: the full 2×2
            // neighborhood must lie inside the source; otherwise black.
            const long shh = levelImg ? levelImg->nr() : sh;
            const long sww = levelImg ? levelImg->nc() : sw;
            if (left < 0 || top < 0 || left + 1 >= sww || top + 1 >= shh) continue;

            const RgbPixel& tl = levelImg ? (*levelImg)(top, left)
                                          : src(srcBaseY + top, srcBaseX + left);
            const RgbPixel& tr = levelImg ? (*levelImg)(top, left + 1)
                                          : src(srcBaseY + top, srcBaseX + left + 1);
            const RgbPixel& bl2 = levelImg ? (*levelImg)(top + 1, left)
                                           : src(srcBaseY + top + 1, srcBaseX + left);
            const RgbPixel& br2 = levelImg ? (*levelImg)(top + 1, left + 1)
                                           : src(srcBaseY + top + 1, srcBaseX + left + 1);
            out(r, c) = BlendBilinear(tl, tr, bl2, br2, lr_frac, tb_frac);
        }
    }
}

} // namespace facelogin
