package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"

	"FaceLoginSetup/internal"

	"github.com/wailsapp/wails/v2/pkg/runtime"
)

// App struct
type App struct {
	ctx context.Context
}

// NewApp creates a new App application struct
func NewApp() *App {
	return &App{}
}

// startup is called when the app starts. The context is saved so we can call runtime methods.
func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	// Set up the embedded FS reference for extraction
	internal.EmbeddedFS = resources
}

// ProgressEvent is sent to the frontend during install/uninstall.
type ProgressEvent struct {
	Percent int    `json:"percent"`
	Step    string `json:"step"`
	Status  string `json:"status"` // "running", "done", "warn", "fail"
	Detail  string `json:"detail,omitempty"`
}

func (a *App) emit(percent int, step, status, detail string) {
	runtime.EventsEmit(a.ctx, "setup:progress", ProgressEvent{
		Percent: percent,
		Step:    step,
		Status:  status,
		Detail:  detail,
	})
}

// GetDefaultPaths returns default install and data paths for the frontend.
func (a *App) GetDefaultPaths() map[string]string {
	return map[string]string{
		"installDir": internal.ReadRegString(REGVAL_INSTALL_PATH, internal.GetDefaultInstallDir()),
	}
}

// PickDirectory opens a native folder picker dialog and returns the selected path.
// Returns empty string if the user cancels.
func (a *App) PickDirectory() string {
	dir, err := runtime.OpenDirectoryDialog(a.ctx, runtime.OpenDialogOptions{
		Title: "选择安装目录",
	})
	if err != nil {
		return ""
	}
	return dir
}

// IsInstalled checks whether FaceLogin is already installed by verifying
// the registry InstallPath exists and the install directory itself exists.
func (a *App) IsInstalled() bool {
	installDir := internal.ReadRegString(REGVAL_INSTALL_PATH, "")
	if installDir == "" {
		return false
	}
	return internal.DirExists(installDir)
}

// Install runs the full installation.
func (a *App) Install(installDir string) map[string]interface{} {
	var err error

	a.emit(0, "开始安装", "running", "")
	installDir = filepath.Clean(installDir)

	// Validate the complete, exact model set before stopping a working prior
	// installation. This also detects a corrupted installer payload early.
	a.emit(0, "校验安装资源", "running", "")
	if err = internal.ValidateEmbeddedResources(); err != nil {
		a.emit(0, "校验安装资源", "fail", err.Error())
		return result(false, fmt.Sprintf("安装资源校验失败: %v", err))
	}
	a.emit(0, "校验安装资源", "done", "")

	// Step 1: Stop and delete existing service
	a.emit(0, "停止并删除已有服务", "running", "")
	if err = internal.StopAndDeleteService(); err != nil {
		return result(false, err.Error())
	}
	a.emit(12, "停止并删除已有服务", "done", "")

	// Step 2: Create directories
	a.emit(12, "创建目标目录", "running", "")
	if err = os.MkdirAll(installDir, 0755); err != nil {
		return result(false, err.Error())
	}
	a.emit(25, "创建目标目录", "done", "")

	// Protect the root before writing executable/model bytes into it. Files
	// extracted below inherit only SYSTEM/Administrators write access, closing
	// the replacement window that would exist if ACLs were applied afterwards.
	a.emit(25, "设置数据目录权限", "running", "")
	if err = internal.SetDirectoryACL(installDir); err != nil {
		a.emit(25, "设置数据目录权限", "fail", err.Error())
		return result(false, fmt.Sprintf("设置安装目录安全权限失败: %v", err))
	}
	a.emit(30, "设置数据目录权限", "done", "")

	// Step 3: Write registry paths — DataPath is the install dir itself, C++ appends \models
	a.emit(30, "配置注册表路径", "running", "")
	if err = internal.WriteRegString(REGVAL_INSTALL_PATH, installDir); err != nil {
		return result(false, fmt.Sprintf("写入安装路径失败: %v", err))
	}
	if err = internal.WriteRegString(REGVAL_DATA_PATH, installDir); err != nil {
		return result(false, fmt.Sprintf("写入数据路径失败: %v", err))
	}
	a.emit(35, "配置注册表路径", "done", "")

	// Lock the registry key's DACL (security #3): now that the DataPath/
	// InstallPath values are written, protect them against non-admin
	// redirection. Failure is fatal — an unprotected key leaves the data
	// directory redirectable, which the C++ allow-list alone only partially
	// mitigates.
	a.emit(36, "保护注册表", "running", "")
	if err = internal.SetRegistryKeyACL(); err != nil {
		a.emit(36, "保护注册表", "fail", err.Error())
		return result(false, fmt.Sprintf("注册表安全保护失败: %v", err))
	}
	a.emit(38, "保护注册表", "done", "")

	// Step 4: Extract all embedded resources
	a.emit(35, "复制文件", "running", "")
	if err = internal.ExtractAll(installDir, func(step, total int, name string) {
		a.emit(35+step*25/total, "复制文件", "running", name)
	}); err != nil {
		return result(false, fmt.Sprintf("复制文件失败: %v", err))
	}
	if err = internal.ValidateInstalledModels(installDir); err != nil {
		a.emit(30, "校验已复制模型", "fail", err.Error())
		return result(false, fmt.Sprintf("模型文件校验失败: %v", err))
	}
	// Step 4.5: Create data/ and log/ directories, ensure config.json defaults
	dataDir := filepath.Join(installDir, "data")
	logDir := filepath.Join(installDir, "log")
	os.MkdirAll(dataDir, 0755)
	os.MkdirAll(logDir, 0755)
	configPath := filepath.Join(dataDir, "config.json")
	// Selectively force the adjusted default parameters (match/anti-spoof
	// thresholds) onto any existing config, and create a default config when
	// none exists. Other user settings are preserved.
	if err := internal.EnsureConfigDefaults(configPath); err != nil {
		a.emit(60, "写入默认设置", "warn", err.Error())
	} else {
		a.emit(60, "写入默认设置", "done", "")
	}

	a.emit(60, "复制文件", "done", "")

	// Re-apply recursively as a postcondition in case an extracted resource
	// carried or acquired an unexpected explicit ACL.
	a.emit(60, "验证目录权限", "running", "")
	if err = internal.SetDirectoryACL(installDir); err != nil {
		a.emit(60, "验证目录权限", "fail", err.Error())
		return result(false, fmt.Sprintf("设置安装目录安全权限失败: %v", err))
	}
	// data\ (users.dat) then gets a tighter ACL with NO Users read entry —
	// after the sweep above, whose /T would otherwise re-grant Users onto the
	// data subtree. Fatal on failure: a Users-readable users.dat defeats the
	// DPAPI machine-scope encryption (any local account could decrypt it).
	if err = internal.SetDataDirectoryACL(dataDir); err != nil {
		a.emit(60, "验证目录权限", "fail", err.Error())
		return result(false, fmt.Sprintf("设置数据目录安全权限失败: %v", err))
	}
	a.emit(67, "验证目录权限", "done", "")

	// Step 6: Register COM DLL
	a.emit(67, "注册登录组件", "running", "")
	dllPath := filepath.Join(installDir, "FaceLoginCredentialProvider.dll")
	if err = internal.RegisterCOMDLL(dllPath); err != nil {
		a.emit(67, "注册登录组件", "fail", err.Error())
		return result(false, err.Error())
	}
	a.emit(75, "注册登录组件", "done", "")

	// Step 7: Install service (stops old service first)
	a.emit(75, "安装并启动认证服务", "running", "")
	svcPath := filepath.Join(installDir, "FaceLoginService.exe")
	if err = internal.InstallService(svcPath); err != nil {
		a.emit(75, "安装并启动认证服务", "fail", err.Error())
		return result(false, err.Error())
	}
	a.emit(90, "安装并启动认证服务", "done", "")

	// Step 8: Finalize
	enrollDest := filepath.Join(installDir, "FaceLoginConsole.exe")
	_ = internal.ExtractResource("resources/FaceLoginConsole.exe", enrollDest)

	a.emit(100, "完成", "done", "")
	// Enrollment is always the user's next step, so open the console right
	// away. The child inherits our elevated token — no second UAC prompt.
	message := "安装完成。\n注册向导已自动打开，请完成人脸注册。"
	if startErr := internal.StartProgram(enrollDest); startErr != nil {
		message = "安装完成。\n自动打开注册向导失败，请手动运行安装目录下的 FaceLoginConsole.exe 注册人脸。"
	}
	return result(true, message)
}

// Uninstall runs the full uninstallation.
func (a *App) Uninstall() map[string]interface{} {
	var err error
	cleanupFailed := false
	rebootRequired := false

	a.emit(0, "开始卸载", "running", "")

	// Step 1: Stop and delete service
	a.emit(0, "停止并删除认证服务", "running", "")
	if err = internal.StopAndDeleteService(); err != nil {
		a.emit(0, "停止并删除认证服务", "fail", err.Error())
		return result(false, err.Error())
	}
	a.emit(30, "停止并删除认证服务", "done", "")

	// Step 2: Unregister COM DLL
	a.emit(30, "注销登录组件", "running", "")
	installDir := internal.ReadRegString(REGVAL_INSTALL_PATH, "")
	if installDir == "" {
		// A previous uninstall may have removed the registry key while a
		// locked file was deferred until reboot. Recover the default install
		// directory so a second uninstall can finish the cleanup.
		defaultDir := internal.GetDefaultInstallDir()
		if internal.IsSafeInstallDir(defaultDir) {
			installDir = defaultDir
		}
	}
	if installDir != "" {
		dllPath := filepath.Join(installDir, "FaceLoginCredentialProvider.dll")
		if err = internal.UnregisterCOMDLL(dllPath); err != nil {
			cleanupFailed = true
			a.emit(30, "注销登录组件", "warn", err.Error())
		} else {
			a.emit(50, "注销登录组件", "done", "")
		}
	} else {
		internal.UnregisterCOMDLL("")
		a.emit(50, "注销登录组件", "done", "")
	}

	// Step 3: Delete installed program files AND user data (data/, log/) and
	// remove the install directory if it becomes empty. This is a FULL purge —
	// enrolled faces, config, and logs are gone (the frontend warns about this
	// before uninstall). The directory itself is only removed after every file
	// we own has been deleted and the folder is confirmed empty, so an
	// unexpected/unknown file (not deployed by us) leaves the dir in place and
	// never gets silently wiped.
	a.emit(50, "删除程序文件", "running", "")
	if installDir != "" && internal.DirExists(installDir) {
		if !internal.IsSafeInstallDir(installDir) {
			cleanupFailed = true
			a.emit(50, "删除程序文件", "warn", "安装目录校验失败，已保留该目录。")
		} else {
			removed, filesPending, rmErr := internal.RemoveInstalledFiles(installDir, true)
			installPending := filesPending
			if rmErr != nil {
				cleanupFailed = true
				a.emit(70, "删除程序文件", "warn",
					fmt.Sprintf("已删除 %d 个文件，部分文件删除失败：%v", removed, rmErr))
			} else if filesPending {
				a.emit(70, "删除程序文件", "warn",
					fmt.Sprintf("已删除 %d 个文件，其余占用文件已安排在重启后删除。", removed))
			} else {
				a.emit(70, "删除程序文件", "done",
					fmt.Sprintf("已删除 %d 个程序文件及用户数据。", removed))
			}

			// Remove the (now empty) install directory — guarded so it only
			// happens when nothing remains.
			if internal.DirExists(installDir) {
				removedDir, dirPending, dirErr := internal.RemoveInstalledDir(installDir)
				installPending = installPending || dirPending
				if dirErr != nil {
					cleanupFailed = true
					a.emit(70, "删除程序文件", "warn",
						fmt.Sprintf("安装目录删除失败：%v", dirErr))
				} else if dirPending {
					a.emit(70, "删除程序文件", "warn",
						"安装目录已验证安排在重启后删除。")
				} else if removedDir {
					// A transient lock may have cleared during finalization. The
					// install tree is gone now, so stale pending-delete entries do
					// not require a reboot.
					installPending = false
				} else if !removedDir {
					cleanupFailed = true
					a.emit(70, "删除程序文件", "warn",
						"安装目录仍有残留文件，且未能安排重启删除。")
				}
			} else {
				installPending = false
			}
			rebootRequired = rebootRequired || installPending
		}
	} else {
		a.emit(70, "删除程序文件", "done", "")
	}

	// Step 4: Remove shared runtime data under %ProgramData%\FaceLogin
	// (config.json, enrolled face database users.dat, logs, model cache). The
	// installer points DataPath at the install dir, so this directory only
	// exists when an older release used it as the default or the app ran
	// standalone — but on such machines it holds real data that must not
	// survive a full uninstall. Done before the registry key is deleted so the
	// data path is still resolvable if this ever switches back to registry.
	a.emit(70, "删除运行数据", "running", "")
	removedData, dataPending, dataErr := internal.RemoveProgramData()
	rebootRequired = rebootRequired || dataPending
	if dataErr != nil {
		cleanupFailed = true
		a.emit(80, "删除运行数据", "warn",
			fmt.Sprintf("运行数据删除失败：%v", dataErr))
	} else if dataPending {
		a.emit(80, "删除运行数据", "warn", "运行数据已安排在重启后删除。")
	} else if removedData {
		a.emit(80, "删除运行数据", "done", "已删除运行数据目录。")
	} else {
		a.emit(80, "删除运行数据", "done", "无运行数据目录。")
	}

	// Step 5: Clean registry — remove the whole HKLM\SOFTWARE\FaceLogin key.
	// The service writes runtime values (e.g. UserLoggedIn) that the installer
	// never created, so deleting only InstallPath/DataPath would leave the key
	// behind.
	a.emit(80, "清理注册表", "running", "")
	if cleanupFailed {
		// Keep InstallPath so a custom install location can be found and
		// cleaned again after the failure is resolved. A default path is only
		// a fallback.
		a.emit(80, "清理注册表", "warn", "保留安装路径，修复问题后可再次运行卸载。")
	} else {
		if regErr := internal.DeleteRegKey(); regErr != nil {
			cleanupFailed = true
			a.emit(80, "清理注册表", "warn", regErr.Error())
		} else {
			a.emit(90, "清理注册表", "done", "")
		}
	}

	// Step 6: Notify complete
	a.emit(90, "完成", "running", "")
	message := "卸载完成，登录界面已恢复为默认密码登录。程序文件、人脸数据和日志已全部删除。"
	status := "done"
	success := true
	if cleanupFailed {
		message = "卸载未完全完成：存在未删除且未能安排重启删除的项目。安装路径已保留，请查看上方错误后重试。"
		status = "fail"
		success = false
	} else if rebootRequired {
		message = "卸载操作已完成；部分被占用文件和目录已验证安排在重启 Windows 后删除。"
		status = "warn"
	}
	a.emit(100, "完成", status, message)

	response := result(success, message)
	response["rebootRequired"] = rebootRequired
	return response
}

func result(success bool, message string) map[string]interface{} {
	return map[string]interface{}{
		"success": success,
		"message": message,
	}
}
