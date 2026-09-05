#pragma once
// face_align.h — 5-point similarity-transform face alignment.
//
// Replaces the dlib 68-point shape predictor + get_face_chip_details path
// (v1.5). SCRFD detects the 5 landmarks used for alignment directly
// (Detection::kps, source-pixel coordinates), so no separate landmark model
// is needed. InsightFace recognition models (w600k_mbf / w600k_r50) are
// trained on faces aligned with a similarity transform to a fixed reference
// frame; this module reproduces that alignment exactly.
//
// Why similarity (not full affine): scale + rotation + translation only.
// Full affine can shear the face, which would push the embedding away from
// the training distribution. dlib's own face_recognition pipeline uses the
// same 5-point similarity convention.

#include "../common/frame_image.h"
#include <algorithm>
#include <cmath>

namespace facelogin {

// A detected face with its 5 SCRFD keypoints. Used by the enrollment
// preview overlay to draw the face box.
struct FaceWithKps {
    FaceRect rect;
    float kps[10];  // 5 points (x,y pairs): left-eye, right-eye, nose,
                    // left-mouth, right-mouth — source-pixel coordinates
};

// InsightFace's standard 112×112 alignment reference frame (UMD template):
//   [0] left eye, [1] right eye, [2] nose, [3] left mouth, [4] right mouth
inline constexpr float kInsightFaceRef112[10] = {
    38.2946f, 51.6963f,
    73.5318f, 51.5014f,
    56.0252f, 71.7366f,
    41.5493f, 92.3655f,
    70.7299f, 92.2041f,
};

// Least-squares similarity transform (rotation + uniform scale +
// translation, no shear) mapping src 5 points onto dst 5 points.
// out is a 2×3 row-major affine matrix [a b c; d e f]:
//   X = a*x + b*y + c
//   Y = d*x + e*y + f
// with d == b, e == a (similarity constraint). Closed-form Umeyama fit.
// Returns false if the source points are degenerate (zero variance).
inline bool EstimateSimilarityTransform(const float src[10], const float dst[10],
                                        float out[6]) {
    // Centroids of source and target.
    float mx = 0, my = 0, mu = 0, mv = 0;
    for (int i = 0; i < 5; i++) {
        mx += src[i * 2];     my += src[i * 2 + 1];
        mu += dst[i * 2];     mv += dst[i * 2 + 1];
    }
    mx /= 5; my /= 5; mu /= 5; mv /= 5;

    // With u' = a*x' - b*y', v' = b*x' + a*y' (a = s·cosθ, b = s·sinθ),
    // least squares over all 5 points gives a closed form:
    float sxx = 0, syy = 0, sxu = 0, syv = 0, sxv = 0, syu = 0;
    for (int i = 0; i < 5; i++) {
        float x = src[i * 2] - mx, y = src[i * 2 + 1] - my;
        float u = dst[i * 2] - mu, v = dst[i * 2 + 1] - mv;
        sxx += x * x; syy += y * y;
        sxu += x * u; syv += y * v;
        sxv += x * v; syu += y * u;
    }
    float denom = sxx + syy;
    if (denom < 1e-6f) return false;   // degenerate: points coincide

    float a = (sxu + syv) / denom;
    float b = (sxv - syu) / denom;

    out[0] = a;  out[1] = -b;  out[2] = mu - a * mx + b * my;
    out[3] = b;  out[4] = a;   out[5] = mv - b * mx - a * my;
    return true;
}

// Warp `image` into a `size`×`size` chip using the affine matrix m
// (inverse mapping with bilinear sampling; out-of-bounds samples are black).
// Handles the general affine inverse, so a stray non-similarity matrix
// degrades gracefully instead of warping incorrectly.
inline void WarpAffine(const FrameImage& image,
                       const float m[6], int size,
                       FrameImage& out) {
    const long srcH = static_cast<long>(image.nr());
    const long srcW = static_cast<long>(image.nc());
    out.set_size(size, size);

    const float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5];
    const float det = a * e - b * d;
    if (std::abs(det) < 1e-6f) {
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                out(y, x) = RgbPixel(0, 0, 0);
        return;
    }

    // Inverse of [a b; d e] is 1/det [e, -b; -d, a]; offset mapped too:
    //   x = (e*(X-c) - b*(Y-f)) / det
    //   y = (-d*(X-c) + a*(Y-f)) / det
    for (int Y = 0; Y < size; Y++) {
        for (int X = 0; X < size; X++) {
            float sx = (e * (X - c) - b * (Y - f)) / det;
            float sy = (-d * (X - c) + a * (Y - f)) / det;
            if (sx < 0 || sy < 0 || sx >= srcW || sy >= srcH) {
                out(Y, X) = RgbPixel(0, 0, 0);
                continue;
            }
            long x0 = static_cast<long>(sx);
            long y0 = static_cast<long>(sy);
            long x1 = std::min(x0 + 1, srcW - 1);
            long y1 = std::min(y0 + 1, srcH - 1);
            float fx = sx - x0;
            float fy = sy - y0;
            const auto& p00 = image(y0, x0);
            const auto& p10 = image(y0, x1);
            const auto& p01 = image(y1, x0);
            const auto& p11 = image(y1, x1);
            auto blend = [&](unsigned char c00, unsigned char c10,
                             unsigned char c01, unsigned char c11) {
                float top = c00 + (c10 - c00) * fx;
                float bot = c01 + (c11 - c01) * fx;
                return static_cast<unsigned char>(top + (bot - top) * fy + 0.5f);
            };
            RgbPixel p;
            p.red   = blend(p00.red,   p10.red,   p01.red,   p11.red);
            p.green = blend(p00.green, p10.green, p01.green, p11.green);
            p.blue  = blend(p00.blue,  p10.blue,  p01.blue,  p11.blue);
            out(Y, X) = p;
        }
    }
}

// High-level alignment: map the face described by kps into a size×size chip
// using the InsightFace reference frame. Returns false if the transform is
// degenerate (e.g. collinear keypoints).
inline bool AlignFace5(const FrameImage& image,
                       const float kps[10], int size,
                       FrameImage& out) {
    float m[6];
    if (!EstimateSimilarityTransform(kps, kInsightFaceRef112, m)) return false;
    WarpAffine(image, m, size, out);
    return true;
}

// Estimate head yaw (degrees) from the 5 keypoints with a weak-perspective
// model: nose-tip displacement from the eye midpoint, normalized by the
// apparent interocular distance, then arctan-scaled.
//
//   yaw ≈ atan(k · noseOffset / eyeDist)
//
// k = eyeDist3D / noseProtrusion3D. A typical adult face has interocular
// ≈ 65 mm and nose-tip protrusion ≈ 35 mm → k ≈ 1.86. k is a calibration
// constant (kYawCalibration): if the estimate reads off at 30°, tune it and
// re-test (multi-angle enrollment gates on this, so it must be honest).
//
// Sign convention (verified against a facing observer): the person turns
// toward their OWN LEFT → the nose moves toward image +x → yaw > 0.
// So "turn left 30°" → yaw ≈ +30°.
inline float EstimateYawDeg(const float kps[10]) {
    const float lex = kps[0], ley = kps[1];   // left eye
    const float rex = kps[2], rey = kps[3];   // right eye
    const float nx  = kps[4];                 // nose

    float midX = (lex + rex) * 0.5f;
    float eyeDist = std::sqrt((rex - lex) * (rex - lex) + (rey - ley) * (rey - ley));
    if (eyeDist < 1e-4f) return 0.0f;

    constexpr float kYawCalibration = 1.86f;  // eyeDist/protrusion ratio (measured)
    return std::atan(kYawCalibration * (nx - midX) / eyeDist) * 180.0f / 3.14159265358979f;
}

// Estimate head pitch (degrees) from the 5 keypoints, same weak-perspective
// family as EstimateYawDeg. The nose tip sits BELOW the eye line even in a
// neutral pose, so the offset must be measured against the canonical
// neutral-pose ratio instead of zero:
//
//   r  = (noseY - eyeMidY) / eyeDist      (nose-below-eye ratio, image y down)
//   r0 = 0.5714                           (kInsightFaceRef112 neutral pose)
//   pitch ≈ atan(k · (r - r0))
//
// Pitching the head forward (chin down) drops the nose tip further below the
// eye line → r > r0 → pitch > 0. k = 1.86 matches yaw: the same
// protrusion/interocular geometry drives both small-angle sensitivities.
// kAngleTargets write nominalPitch = 0, so only the CONSISTENCY of this
// estimator with itself matters for the ±25° learning cone (docs/
// progressive-learning-v2.md §3), not absolute accuracy.
inline float EstimatePitchDeg(const float kps[10]) {
    const float ley = kps[1];              // left eye y
    const float rey = kps[3];              // right eye y
    const float ny  = kps[5];              // nose y

    float midY = (ley + rey) * 0.5f;
    float eyeDist = std::sqrt((kps[2] - kps[0]) * (kps[2] - kps[0]) +
                              (rey - ley) * (rey - ley));
    if (eyeDist < 1e-4f) return 0.0f;

    constexpr float kNeutralNoseRatio = 0.5714f;  // kInsightFaceRef112
    constexpr float kPitchCalibration = 1.86f;
    const float r = (ny - midY) / eyeDist;
    return std::atan(kPitchCalibration * (r - kNeutralNoseRatio)) *
           180.0f / 3.14159265358979f;
}

// A stored/derived pose angle is usable only when finite and inside the
// physical ±90° range. Anything else (notably credential_store's
// kNominalAngleInvalid sentinel carried by V4-era records) means "no pose
// information" — consumers treat that as pass, never as 0°.
inline bool IsValidNominalAngle(float v) {
    return std::isfinite(v) && std::fabs(v) <= 90.0f;
}

} // namespace facelogin
