#pragma once

#include "../common/frame_image.h"
#include <string>

struct IWICImagingFactory;

namespace facelogin {

// Construct/destroy on the UI's existing COM apartment, with Encode called
// by one preview thread at a time. The host joins that thread before disposal.
// This module owns WIC objects but never initializes/uninitializes COM.
class PreviewFrameEncoder {
public:
    PreviewFrameEncoder();
    ~PreviewFrameEncoder();
    PreviewFrameEncoder(const PreviewFrameEncoder&) = delete;
    PreviewFrameEncoder& operator=(const PreviewFrameEncoder&) = delete;

    // Produces the existing JPEG (quality 0.70) data URI. Failure clears out.
    bool Encode(const FrameImage& frame, std::string& out);

private:
    IWICImagingFactory* m_factory = nullptr;
};

} // namespace facelogin
