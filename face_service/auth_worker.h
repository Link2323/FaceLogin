#pragma once

#include <windows.h>

namespace facelogin {

// Private entry point used only by FaceLoginService.exe -auth-worker. The
// caller supplies inherited anonymous-pipe handles; the worker never creates
// a public listener and never loads the credential database.
int RunAuthenticationWorker(HANDLE parentToWorker, HANDLE workerToParent);

} // namespace facelogin
