// Desktop-only diagnostic: CSV of distinct frames after sensor writes.
// Does not save images or modify camera_tune.state. Restores values AND modes.
#include "webcam_capture_dshow.h"
#include "exposure_warmup.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace facelogin;
using Clock = std::chrono::steady_clock;

struct RestoreControls {
    WebcamCaptureDS& camera;
    long exposure = 0, gain = 0;
    bool exposureManual = false, gainManual = false;
    bool haveExposure = false, haveGain = false;
    bool Restore() {
        const bool gainOk = !haveGain || camera.SetGain(gain, gainManual);
        const bool exposureOk = !haveExposure || camera.SetExposure(exposure, exposureManual);
        haveGain = haveExposure = false;
        return gainOk && exposureOk;
    }
    ~RestoreControls() {
        if (!Restore()) std::fprintf(stderr, "WARNING: restoring original camera controls failed\n");
    }
};

int wmain(int argc, wchar_t** argv) {
    if (!WebcamCaptureDS::InitializeCOM()) return 1;
    struct ComGuard { ~ComGuard() { WebcamCaptureDS::ShutdownCOM(); } } com;
    const auto cameras = WebcamCaptureDS::ListCameras();
    if (argc == 2 && std::wstring(argv[1]) == L"--list") {
        for (size_t i = 0; i < cameras.size(); ++i)
            std::wcout << i << L": " << cameras[i].friendlyName << L"\n" << cameras[i].devicePath << L"\n";
        return 0;
    }
    if (argc != 3 || std::wstring(argv[1]) != L"--camera-index") {
        std::fprintf(stderr, "Usage: ExposureResponseProbe --list | --camera-index N\n");
        return 2;
    }
    wchar_t* end = nullptr;
    const long index = std::wcstol(argv[2], &end, 10);
    if (!argv[2][0] || *end || index < 0 || static_cast<size_t>(index) >= cameras.size()) return 2;
    WebcamCaptureDS camera;
    if (!camera.Initialize(640, 480, cameras[index].devicePath)) return 1;
    RestoreControls restore{camera};
    restore.haveExposure = camera.GetExposure(&restore.exposure, &restore.exposureManual);
    restore.haveGain = camera.GetGain(&restore.gain, &restore.gainManual);
    long emin = 0, emax = 0, estep = 0, gmin = 0, gmax = 0, gstep = 0;
    if (!restore.haveExposure || !camera.GetExposureRange(&emin, &emax, &estep) || estep <= 0) return 1;
    std::fprintf(stderr, "Original exposure=%ld manual=%d gain=%ld manual=%d; exposure range=%ld..%ld step=%ld\n",
                 restore.exposure, restore.exposureManual, restore.gain, restore.gainManual, emin, emax, estep);
    unsigned long long lastSeq = 0;
    std::puts("stage,cycle,ms,seq,mean,center,clipped_fraction,direction");
    const auto sample = [&](const char* stage, int cycle, int durationMs, int direction = 0) {
        const auto start = Clock::now();
        int samples = 0;
        while (Clock::now() - start < std::chrono::milliseconds(durationMs)) {
            FrameImage frame;
            unsigned long long seq = 0;
            if (camera.GrabFrame(frame, &seq) && seq > lastSeq && !frame.is_empty()) {
                lastSeq = seq;
                double central = 0;
                int count = 0, clipped = 0;
                for (long y = frame.nr() / 4; y < frame.nr() * 3 / 4; y += 4)
                    for (long x = frame.nc() / 4; x < frame.nc() * 3 / 4; x += 4) {
                        const auto& p = frame(y, x);
                        const double luma = .299 * p.red + .587 * p.green + .114 * p.blue;
                        central += luma;
                        ++count;
                        if (luma >= 245) ++clipped;
                    }
                std::printf("%s,%d,%.2f,%llu,%.3f,%.3f,%.4f,%d\n", stage, cycle,
                    std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                    seq, MeanLuma(frame), count ? central / count : 0,
                    count ? static_cast<double>(clipped) / count : 0, direction);
                ++samples;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return samples >= 3;
    };
    if (!camera.SetExposure(restore.exposure) ||
        (restore.haveGain && !camera.SetGain(restore.gain)) || !sample("baseline", 0, 2500)) return 1;
    const long alternate = restore.exposure - estep >= emin ? restore.exposure - estep : restore.exposure + estep;
    if (alternate > emax) return 1;
    for (int cycle = 1; cycle <= 3; ++cycle) {
        const int direction = alternate > restore.exposure ? 1 : -1;
        if (!camera.SetExposure(alternate) || !sample("exposure_step", cycle, 2200, direction) ||
            !camera.SetExposure(restore.exposure) || !sample("exposure_restore", cycle, 2200, -direction)) return 1;
    }
    if (restore.haveGain && camera.GetGainRange(&gmin, &gmax, &gstep) && gstep > 0) {
        const long move = std::max(gstep, ((gmax - gmin) / 8 / gstep) * gstep);
        const long otherGain = restore.gain + move <= gmax ? restore.gain + move : std::max(gmin, restore.gain - move);
        std::fprintf(stderr, "Gain probe %ld->%ld\n", restore.gain, otherGain);
        for (int cycle = 1; cycle <= 3 && otherGain != restore.gain; ++cycle) {
            const int direction = otherGain > restore.gain ? 1 : -1;
            if (!camera.SetGain(otherGain) || !sample("gain_step", cycle, 2200, direction) ||
                !camera.SetGain(restore.gain) || !sample("gain_restore", cycle, 2200, -direction)) return 1;
        }
    }
    const bool restored = restore.Restore();
    std::fprintf(stderr, "Original controls restored: %s\n", restored ? "yes" : "NO");
    return restored ? 0 : 1;
}
