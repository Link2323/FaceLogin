#pragma once

#include "auth_worker_protocol.h"

#include "../common/frame_image.h"

#include <memory>
#include <string>

namespace facelogin {

// Common worker-owned camera boundary. The parent service never constructs an
// implementation of this interface.
class ICameraBackend {
public:
    virtual ~ICameraBackend() = default;
    virtual bool Initialize(int width, int height,
                            const std::wstring& devicePath) = 0;
    virtual bool GrabFrame(FrameImage& frame) = 0;
    virtual void RequestShutdown() = 0;
    virtual void Shutdown() = 0;
};

std::unique_ptr<ICameraBackend> CreateCameraBackend(
    auth_worker::CameraBackend backend);
const wchar_t* CameraBackendName(auth_worker::CameraBackend backend);

} // namespace facelogin
