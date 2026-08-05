@echo off
setlocal enabledelayedexpansion

REM ============================================================================
REM Admin self-check -- writing to %SystemRoot%\System32 and regsvr32 (HKLM)
REM both require elevation.  Without this check the copy/regsvr32 below fail
REM silently, the script keeps going, deploys nothing, and the user tests
REM stale code.  Fail loud instead.
REM (NOTE: keep this file pure-ASCII -- cmd.exe on a GBK codepage chokes on
REM  multi-byte UTF-8 chars like em-dash and the whole script silently mis-parses.)
REM ============================================================================
net session >nul 2>&1
if %errorlevel% neq 0 (
    title [ERROR] FaceLogin - Administrator rights required
    echo.
    echo ================================================================
    echo  [ERROR] This script must be run as Administrator.
    echo ================================================================
    echo.
    echo  It needs to copy the credential provider DLL into
    echo  %SystemRoot%\System32 and register it HKLM-wide, both of
    echo  which require elevation.
    echo.
    echo  How to fix:
    echo    1^) Right-click start_standalone.bat
    echo    2^) Choose "Run as administrator"
    echo.
    echo  Aborting.
    echo.
    pause
    exit /b 1
)

echo ============================================
echo   FaceLogin - Start Service in Standalone Mode
echo ============================================
echo.

REM ---- [1/4] Kill ALL existing instances (incl. zombies) ----
REM We kill both the standalone foreground process AND any installed Windows
REM service instance, because FaceLoginService holds a global singleton mutex
REM ("Global\FaceLoginService_SingleInstance") -- if any instance is alive,
REM the new one prints "already running" and exits 0 immediately, which from
REM a .bat looks exactly like a successful startup followed by an instant exit
REM (window closes after the final pause). Killing first prevents that.
echo [1/4] Killing all existing FaceLoginService instances...
taskkill /F /IM FaceLoginService.exe >nul 2>&1
if !errorlevel! equ 0 (
    echo   Killed running instance^(s^).
) else (
    echo   No running instance found ^(ok^).
)
REM Also stop the installed service (if any) -- ignore errors if not installed.
sc stop FaceLoginService >nul 2>&1
timeout /t 2 /nobreak >nul

REM ---- [2/4] Copy credential provider DLL ----
echo [2/4] Copying credential provider DLL to System32...
set "SRC=%~dp0..\build\credential_provider\Release\FaceLoginCredentialProvider.dll"
set "DST=%SystemRoot%\System32\FaceLoginCredentialProvider.dll"
if not exist "%SRC%" (
    echo   [ERROR] Source DLL not found:
    echo          %SRC%
    echo          Build the solution first ^(Release/x64^).
    echo.
    pause
    exit /b 1
)
copy /Y "%SRC%" "%DST%" >nul
if !errorlevel! neq 0 (
    echo   [ERROR] Failed to copy DLL to System32 ^(error !errorlevel!^).
    echo          If a lock screen / LogonUI is holding the old DLL, log off
    echo          fully or reboot so the new DLL can replace it.
    echo.
    pause
    exit /b 1
)
echo   Copied DLL -^> %DST%

REM ---- [3/4] Register credential provider ----
echo [3/4] Registering credential provider DLL...
regsvr32 /s "%DST%"
if !errorlevel! equ 0 (
    echo   Registered successfully.
) else (
    echo   [ERROR] regsvr32 returned error !errorlevel!.
    echo.
    pause
    exit /b 1
)

REM ---- [4/4] Start service in foreground ----
echo.
echo ============================================
echo [4/4] Starting FaceLoginService in standalone mode.
echo Keep this window open while testing lock screen.
echo Press Ctrl+C to stop.
echo ============================================
echo.

set "SVC=%~dp0..\build\face_service\Release\FaceLoginService.exe"
if not exist "%SVC%" (
    echo [ERROR] Service executable not found:
    echo         %SVC%
    echo         Build the solution first ^(Release/x64^).
    echo.
    pause
    exit /b 1
)

REM Run synchronously. If the service exits immediately (singleton mutex
REM conflict, or Initialize() failure), cmd returns here right away and we
REM report it instead of silently ending.
"%SVC%" -standalone
set "EXITCODE=!errorlevel!"

echo.
if %EXITCODE% neq 0 (
    echo ================================================================
    echo  [WARNING] FaceLoginService exited with code %EXITCODE%.
    echo  If it printed "already running", another instance holds the
    echo  singleton mutex -- re-run this script ^(it kills instances first^).
    echo  Otherwise check the service log:
    echo    %ProgramData%\FaceLogin\log\service.log
    echo ================================================================
) else (
    echo FaceLoginService stopped ^(exit code 0^).
)

echo.
pause
