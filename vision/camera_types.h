#pragma once

#include <string>
#include <vector>

namespace facelogin {

// A video capture device. devicePath is the stable symbolic link
// (e.g. "\\?\usb#vid_046d...") reported by DirectShow. friendlyName is
// the human-readable name shown in the enrollment UI.
struct CameraDeviceInfo {
    std::wstring devicePath;   // stable identifier (symbolic link)
    std::wstring friendlyName; // display name
};

} // namespace facelogin
