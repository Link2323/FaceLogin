package main

import (
	"embed"
	"fmt"
	"os"

	"FaceLoginSetup/internal"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
)

//go:embed all:frontend/dist
var assets embed.FS

// appVersion is shown in Windows' Add/Remove Programs list. Keep in sync
// with wails.json Info.productVersion, build/windows/info.json and the
// README badge.
const appVersion = "1.7.5"

const SERVICE_NAME = "FaceLoginService"

// Registry paths
const REG_KEY = `SOFTWARE\FaceLogin`
const REGVAL_DATA_PATH = "DataPath"
const REGVAL_INSTALL_PATH = "InstallPath"

// startupUninstall lands the UI on the uninstall page instead of install
// (full installer launched with --uninstall; the slim uninstaller build is
// always in uninstall mode via uninstallerBuild).
var startupUninstall = false

func main() {
	for _, arg := range os.Args[1:] {
		if arg == "--uninstall" {
			startupUninstall = true
		}
	}

	// Check administrator — if not elevated, relaunch as admin
	if !internal.IsAdmin() {
		err := internal.Elevate()
		if err != nil {
			fmt.Fprintf(os.Stderr, "Elevation failed: %v\n", err)
		}
		os.Exit(0)
	}

	// Initialize the embedded resource filesystem in the internal package.
	// In the slim build this is an empty FS — install is unavailable there.
	internal.EmbeddedFS = resources

	app := NewApp()

	err := wails.Run(&options.App{
		Title:            "FaceLogin 安装程序",
		Width:            640,
		Height:           520,
		WindowStartState: options.Normal,
		AssetServer: &assetserver.Options{
			Assets: assets,
		},
		BackgroundColour: &options.RGBA{R: 255, G: 255, B: 255, A: 1},
		OnStartup:        app.startup,
		Bind: []interface{}{
			app,
		},
	})
	if err != nil {
		println("Error:", err.Error())
	}
}
