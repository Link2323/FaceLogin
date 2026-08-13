#include "camera_backend.h"

#include "webcam_capture.h"
#include "webcam_capture_dshow.h"

namespace facelogin {
namespace {

class DirectShowBackend final : public ICameraBackend {
public:
    bool Initialize(int width, int height,
                    const std::wstring& devicePath) override {
        return m_camera.Initialize(width, height, devicePath);
    }
    bool GrabFrame(FrameImage& frame) override {
        return m_camera.GrabFrame(frame);
    }
    void RequestShutdown() override {
        // DirectShow capture is callback-based rather than blocked in a
        // synchronous read. Stopping the graph is its interruption primitive.
        m_camera.Pause();
    }
    void Shutdown() override { m_camera.Shutdown(); }

private:
    WebcamCaptureDS m_camera;
};

class MediaFoundationBackend final : public ICameraBackend {
public:
    MediaFoundationBackend()
        : m_prepared(WebcamCapture::InitializeMF()) {}

    ~MediaFoundationBackend() override {
        m_camera.Shutdown();
        if (m_prepared) WebcamCapture::ShutdownMF();
    }

    bool Initialize(int width, int height,
                    const std::wstring& devicePath) override {
        return m_camera.Initialize(width, height, devicePath);
    }
    bool GrabFrame(FrameImage& frame) override {
        return m_camera.GrabFrame(frame);
    }
    void RequestShutdown() override { m_camera.RequestShutdown(); }
    void Shutdown() override { m_camera.Shutdown(); }

private:
    // Creating the backend during worker preload performs MFStartup only. The
    // media source and SourceReader are not activated until AUTH_START calls
    // Initialize(), so the camera indicator remains off while locked.
    bool m_prepared = false;
    WebcamCapture m_camera;
};

} // namespace

std::unique_ptr<ICameraBackend> CreateCameraBackend(
    auth_worker::CameraBackend backend) {
    if (backend == auth_worker::CameraBackend::DirectShow) {
        return std::make_unique<DirectShowBackend>();
    }
    if (backend == auth_worker::CameraBackend::MediaFoundation) {
        return std::make_unique<MediaFoundationBackend>();
    }
    return {};
}

const wchar_t* CameraBackendName(auth_worker::CameraBackend backend) {
    switch (backend) {
    case auth_worker::CameraBackend::DirectShow: return L"dshow";
    case auth_worker::CameraBackend::MediaFoundation: return L"mf";
    default: return L"unknown";
    }
}

} // namespace facelogin
