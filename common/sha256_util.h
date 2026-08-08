#pragma once

// SHA-256 file hashing for runtime model integrity verification.
//
// The installer validates every bundled ONNX model against a hard-coded SHA-256
// manifest at install time (see installer/FaceLoginSetup/internal/extract.go).
// That check runs once. Afterwards any admin process can replace a model file
// on disk — and since the whole bug3 fail-closed liveness defense is built on
// "the PAD model is trustworthy", a swapped model that always returns real≥0.99
// would silently defeat photo-attack protection. This module lets the service
// re-verify each model against the same manifest on every load, so a tampered
// model is rejected fail-closed just like a missing or corrupt one.
//
// Implementation uses the Windows CryptoAPI (wincrypt.h, crypt32.dll) — zero
// third-party dependencies, already linked by facelogin_common. The routine is
// adapted from tools/pad_calibration/main.cpp::FileSha256 and made shared here
// so both the service and the console app use one authoritative copy.

#include <filesystem>
#include <optional>
#include <string>

namespace facelogin {

// Computes the SHA-256 of the file at `path` and returns it as a lowercase
// hexadecimal string. Returns std::nullopt on any I/O or crypto failure
// (missing file, unreadable, CryptoAPI unavailable). Never throws.
std::optional<std::string> FileSha256(const std::filesystem::path& path);

// Verifies a model file's SHA-256 against `expectedHex` (lowercase hex).
// Returns true on match, false on mismatch or hash failure. On failure, logs
// the path, the expected hash, and (if computable) the actual hash via
// FACELOGIN_ERROR, so a tampered model is immediately diagnosable in logs.
//
// `modelLabel` is a short human-readable name (e.g. "SCRFD detector") used in
// the log message to identify which model failed verification.
//
// Case-insensitive comparison: tolerates either case in `expectedHex`, though
// the canonical manifest values are lowercase.
bool VerifyModelIntegrity(const std::filesystem::path& path,
                          const std::string& expectedHex,
                          const wchar_t* modelLabel);

}  // namespace facelogin
