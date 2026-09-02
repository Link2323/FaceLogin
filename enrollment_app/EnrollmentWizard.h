#pragma once

#include <windows.h>
#include <wincodec.h>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

#include "../common/frame_image.h"
#include "../face_service/face_align.h"
#include "../face_service/liveness_types.h"
#include "../face_service/onnx_models.h"
#include "../face_service/webcam_capture_dshow.h"
#include "../face_service/credential_store.h"
#include "../common/config_util.h"

namespace facelogin {

// Backend controller for face enrollment.
// All UI is handled by WebView2 + HTML; this class provides data & actions
// to JavaScript via a COM host object.
class EnrollmentWizard {
public:
    EnrollmentWizard();
    ~EnrollmentWizard();

    // === Called from JS via host object ===

    bool StartPreview();
    void StopPreview();
    int  GetSampleCount() const { return m_samplesCollected.load(); }
    std::string GetUsername() const;
    std::string GetUserSid() const;
    std::string GetAccountType() const { return m_accountType; }
    // Blocking multi-angle capture for one head-yaw position:
    //   0 = front (0°), 1 = left (+30°), 2 = right (−30°).
    // Each call collects 5 yaw-gated frames (|yaw − target| ≤ 10°, face ≥ 60px,
    // detection score ≥ 0.5); JS drives 0 → 1 → 2 in sequence. Angle 0 also
    // runs the liveness check first. Frames accumulate across calls (flat
    // m_embeddings, grouped in angle order), so SaveEnrollment can create one
    // face record per angle without ever averaging across angles.
    bool CaptureFaceSamples(int angleIndex);
    // Cooperative stop: both capture phases check m_capturing between frames,
    // so the thread exits within one poll interval. No-op when not capturing.
    void CancelCapture();
    // Capture progress for the JS UI:
    // {"angle":0,"label":"正面","targetYaw":0,"collected":3,"target":5,
    //  "yaw":12.3,"total":8,"livenessChecking":false,"livenessPassed":true,
    //  "cancelled":false,"done":false}
    std::string GetCaptureStatus();
    bool IsLivenessPassed() const { return m_livenessPassed; }
    bool IsLivenessChecking() const { return m_livenessChecking; }
    bool ValidatePassword(const std::wstring& password);
    // Save enrollment with the given password. label names this face (may be
    // empty — the backend falls back to L"脸N").
    bool SaveEnrollment(const std::wstring& password, const std::wstring& label = L"");

    // Multi-face management (1.3.0). The current account can enroll several
    // faces; each save appends one face instead of replacing the old one.
    // Number of faces enrolled for the current account (0 = not enrolled).
    int GetFaceCount();
    // JSON list of the current account's faces: [{"id":1,"label":"脸1"},...]
    std::string GetFacesJson();
    // Append a new face for the current account without re-entering a password
    // (identity proven by the session token SID). label may be empty.
    bool SaveEnrollmentAppend(const std::wstring& label = L"");
    // Delete one face of the current account (removes the account if it was
    // the last face). Returns false on unknown id / not enrolled.
    bool DeleteFace(int faceId);
    // Remove all faces of the current account (= remove the account).
    bool ClearAllFaces();
    // Rename one face of the current account.
    bool RenameFace(int faceId, const std::wstring& label);

    // Account-type change detection (symmetric MSA ↔ local conversions).
    // Windows keeps the same SID when a user converts their account between a
    // Microsoft account (MSA) and a local account, so the stored record is
    // matched by SID but may carry stale identity/password from the previous
    // account type:
    //   MSA→local:  record UPN still an MSA email, password is the old MSA one.
    //   local→MSA:  record UPN empty (local-era), password is the old local one.
    // Returns 0 = normal / nothing to refresh,
    //         1 = stale MSA→local record detected,
    //         2 = stale local→MSA record detected (current session is an MSA
    //             but the record UPN is empty or differs from the current email).
    int GetAccountTypeChanged();
    // JSON wrapper for JS: {"state":0} | {"state":1,"faces":N} |
    // {"state":2,"faces":N,"upn":"user@mail.com"}.
    std::string CheckAccountTypeChanged();
    // Validate the CURRENT password, then rewrite the stale record IN PLACE
    // (preserving all faces): state 1 clears the old MSA UPN (local account),
    // state 2 writes the current MSA email; both refresh username/SID and
    // re-encrypt the password via DPAPI. Refuses (returns false) unless
    // GetAccountTypeChanged()!=0 and the password validates.
    bool RefreshAccountIdentity(const std::wstring& password);

    // Light-weight dismiss path for the stale-account prompt. State 1 only:
    // clears the bogus MSA email from the current local account's record
    // WITHOUT validating or re-encrypting the password (no input needed).
    // Faces and the stored password are preserved; the lock-screen credential
    // then packs with domain\username instead of the misattributed email.
    // Returns false unless the record is in state 1.
    bool ClearStaleAccountUpn();

    // One-shot startup repair: if the current session is an MSA (per
    // ResolveSessionUpn) but the stored record for THIS session's SID still
    // has an empty UPN, write the resolved MSA email in place — preserving
    // username, SID, faces and the stored (already DPAPI-encrypted) password.
    //
    // Scope is deliberately narrow: only the current session user's own
    // record, and only when its UPN is empty. A non-empty but DIFFERENT UPN
    // may be a genuine re-binding (a user who switched Microsoft accounts) and
    // is left to RefreshAccountIdentity, which requires a password check.
    //
    // Without this, accounts enrolled while GetUserNameExW returned 1332
    // (no UPN) carry an empty UPN forever: the lock-screen credential then
    // packs with domain\username instead of routing through CloudAP, and
    // GetAccountTypeChanged keeps flagging a phantom local→MSA conversion.
    // Returns true iff a record was actually modified.
    bool AutoRepairEmptyUpnOnStartup();

    // Configuration
    std::string GetConfig() const;
    bool SetConfig(const std::string& json);
    bool RestartPreview();

    // Camera device enumeration for the settings UI.
    // Returns JSON: [{path, name}, ...] — path is the stable symbolic link,
    // name is the friendly display name.
    std::string GetCameraList();

    // Log viewer
    std::string GetLogLines();
    std::string GetServiceLogLines();
    std::string GetAuthWorkerLogLines();
    std::string GetCredentialProviderLogLines();
    void ClearLog();
    bool ClearFileLog(const std::wstring& source);

    // Per-frame data for JS canvas rendering (pull model — JS calls these from rAF)
    std::string GetLatestFrameBase64(); // JPEG base64, ~200KB
    std::string GetLatestFacesJson();   // [{x,y,w,h,landmarks:[{x,y},...]},...]
    // Atomically returns "<frame base64>\x1E<faces json>" from the SAME frame.
    std::string GetLatestFrameAndFaces();

    bool IsPreviewRunning() const { return m_previewRunning; }

private:
    std::string EncodeJPEGBase64(const FrameImage& frame);
    std::string FacesToJson(const std::vector<facelogin::FaceWithKps>& faces);

    // Reads <m_dataDir>\log\<logFileName> and returns a JSON array of lines
    // for the JS log viewer (shared by Service/AuthWorker/CredentialProvider).
    std::string ReadLogFileLines(const std::wstring& logFileName);
    // Collapses <m_dataDir>\log\<fileName> to zero length (ClearLog /
    // ClearFileLog). Missing file counts as success.
    bool TruncateLogFileByName(const std::wstring& fileName);

    bool SaveEnrollmentImpl(const std::wstring& password,
                            const std::wstring& label);
    static std::wstring GetCurrentProcessUserSid();

    // Camera & face processing
    std::unique_ptr<WebcamCaptureDS>  m_webcam;
    std::unique_ptr<OnnxDetector>   m_onnxDetector;   // SCRFD detection (+5 keypoints)
    std::unique_ptr<OnnxRecognizer> m_onnxRecognizer; // InsightFace recognition
    std::unique_ptr<OnnxAntiSpoof>  m_antiSpoof;
    CredentialStore m_store;

    // Configuration
    AppConfig m_config;
    float m_antiSpoofThreshold = 0.281f;

    // Frame-grab thread (runs off UI thread — GrabFrame + JPEG encode + detection)
    std::thread m_frameThread;
    std::atomic<bool> m_frameRunning{false};

    // WIC factory (created once)
    IWICImagingFactory* m_wicFactory = nullptr;

    // Per-frame caches (produced by frame thread, consumed by JS on UI thread)
    std::mutex  m_frameCacheMutex;
    std::string m_latestFrameB64;
    std::string m_latestFacesJson;
    FrameImage m_latestFrame;                      // for capture to read
    std::uint64_t m_latestFrameSequence = 0;       // guarded by m_frameCacheMutex

    // Preview state
    bool m_previewRunning = false;

    // Enrollment state
    std::vector<std::vector<float>> m_embeddings;
    std::atomic<int> m_samplesCollected{0};
    std::atomic<bool> m_capturing{false};
    // Set together with m_capturing=false by CancelCapture; lets
    // GetCaptureStatus (and the logs) distinguish a user cancel from a
    // liveness failure until the next CaptureFaceSamples resets it.
    std::atomic<bool> m_captureCancelled{false};
    std::atomic<bool> m_livenessPassed{false};
    std::atomic<bool> m_livenessChecking{false};
    std::thread m_captureThread;

    // Multi-angle enrollment state (v1.5): per-angle sample counts (the flat
    // m_embeddings vector is grouped in angle order), current angle, and the
    // latest yaw estimate for the live UI readout.
    std::atomic<int> m_angleSampleCounts[3] = {0, 0, 0};
    std::atomic<int> m_captureAngle{0};
    std::atomic<float> m_lastYaw{0.0f};
    static constexpr int kAngleTargetFrames = 5;
    static constexpr int kAngleTargets[3] = {0, 30, -30};  // 正面 / 左转 / 右转

    std::wstring m_username;
    std::wstring m_upn;       // UserPrincipalName (e.g. "john@outlook.com") or empty for local
    std::wstring m_sid;       // Security Identifier string
    std::string m_accountType; // "local" or "msa"
    std::wstring m_dataDir;
};

} // namespace facelogin
