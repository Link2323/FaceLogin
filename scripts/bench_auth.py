# Minimal named-pipe client to trigger a real auth request against the
# standalone service and print responses. Used to benchmark per-frame
# detect/embed timings (see "Frame perf:" lines in the service log).
#
# Usage: python bench_auth.py [timeout_seconds]
# The service must be running in -standalone mode.

import ctypes
import sys
import time
from ctypes import wintypes

PIPE_NAME = r"\\.\pipe\FaceLoginPipe"
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


def main():
    timeout = float(sys.argv[1]) if len(sys.argv) > 1 else 16.0

    h = kernel32.CreateFileW(
        PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
    if h == wintypes.HANDLE(-1).value or not h:
        print(f"FAILED to open pipe: WinError {ctypes.get_last_error()}")
        sys.exit(1)
    print("pipe opened")

    # AUTH_REQUEST as UTF-16LE, null-terminated
    msg = ("AUTH_REQUEST" + "\0").encode("utf-16-le")
    written = wintypes.DWORD(0)
    ok = kernel32.WriteFile(h, msg, len(msg), ctypes.byref(written), None)
    if not ok:
        print(f"WriteFile failed: WinError {ctypes.get_last_error()}")
        sys.exit(1)
    print(f"sent AUTH_REQUEST ({written.value} bytes)")

    deadline = time.time() + timeout
    buf = ctypes.create_string_buffer(4096)
    while time.time() < deadline:
        read = wintypes.DWORD(0)
        ok = kernel32.ReadFile(h, buf, 4096, ctypes.byref(read), None)
        if not ok:
            err = ctypes.get_last_error()
            if err == 109:  # ERROR_BROKEN_PIPE
                print("pipe closed by service")
                break
            print(f"ReadFile failed: WinError {err}")
            break
        if read.value:
            text = buf.raw[:read.value].decode("utf-16-le", errors="replace").rstrip("\0")
            print(f"[{time.time():.1f}] {text}")
        time.sleep(0.05)

    kernel32.CloseHandle(h)
    print("done")


if __name__ == "__main__":
    main()
