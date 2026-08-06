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

//go:embed all:resources
//go:embed resources/models/det_10g_gnkps.onnx
//go:embed resources/models/w600k_r50.onnx
//go:embed resources/models/MiniFASNetV2.onnx
//go:embed resources/models/MiniFASNetV1SE.onnx
var resources embed.FS

const SERVICE_NAME = "FaceLoginService"

// Registry paths
const REG_KEY = `SOFTWARE\FaceLogin`
const REGVAL_DATA_PATH = "DataPath"
const REGVAL_INSTALL_PATH = "InstallPath"

func main() {
	// Check administrator — if not elevated, relaunch as admin
	if !internal.IsAdmin() {
		err := internal.Elevate()
		if err != nil {
			fmt.Fprintf(os.Stderr, "Elevation failed: %v\n", err)
		}
		os.Exit(0)
	}

	// =========================================================================
	// Custom action area — PER-RELEASE upgrade actions.
	//
	// Two independent, version-scoped switches live here:
	//
	//   A) Config upgrade  — force-sync changed default parameters onto
	//      existing installs (see internal/config.go).
	//   B) Upgrade notice  — show a "what's new" popup after an upgrade
	//      install completes (see internal/notice.go). Only shown when the
	//      install is an upgrade (a previous version is already installed),
	//      never on a fresh first-time install.
	//
	// Both default to OFF. Each release that needs an action turns the
	// relevant switch ON here; future releases leave them OFF so stale
	// overrides/announcements never re-apply.
	//
	//   internal.ConfigUpgradeEnabled = true   // A: sync thresholds
	//   internal.ConfigUpgradeForcedDefaults = map[string]any{
	//       "match_threshold":      0.30,   // threshold changed in this release
	//       "anti_spoof_threshold": 0.281,
	//   }
	//
	//   internal.NoticeEnabled  = true            // B: announcement popup
	//   internal.NoticeVersion  = "1.2.0"         // badge shown in the popup
	//   internal.NoticeTitle    = "FaceLogin 1.2.0 更新说明"
	//   internal.NoticeBody     = "行1\n行2\n行3"  // one bullet per line
	//
	// v1.0.1: threshold defaults changed (match strictness 70 / anti-spoof 0.30).
	// v1.2.0: no forced config overrides needed (thresholds unchanged; the new
	// camera_device field defaults to "" = first device automatically). The
	// V2→V3 database migration cannot be automated (requires re-enrollment),
	// so it is surfaced via the upgrade notice below instead.
	// v1.3.0: no forced config overrides needed. The V3→V4 database migration
	// (multi-face support) is fully backward compatible — old data is upgraded
	// in memory on load, no re-enrollment required.
	// v1.4.0: no forced config overrides needed. Removed the unused legacy dlib
	// models (recognizer + HOG detector) — smaller installer, no re-enrollment.
	// Dual-MiniFAS migration: scores are not comparable with the removed PAD
	// model, so force the locally calibrated fusion threshold once on upgrade.
	internal.ConfigUpgradeEnabled = true
	internal.ConfigUpgradeForcedDefaults = map[string]any{
		"anti_spoof_threshold": 0.281,
		"liveness_method":      "antispoof",
	}

	// =========================================================================
	// B) Upgrade notice — per-release announcement shown only on UPGRADE.
	// =========================================================================
	internal.NoticeEnabled = true
	internal.NoticeVersion = "1.4.0"
	internal.NoticeTitle = "FaceLogin 1.4.0 更新说明"
	internal.NoticeBody = "安全升级：\n" +
		"- 活体检测升级为 MiniFASNetV2 + MiniFASNetV1SE 双模型融合，显著加强照片与屏幕攻击拦截\n" +
		"- 登录和每个录入角度均要求连续 5 帧全部通过；模型缺失、损坏或推理异常时拒绝认证\n" +
		"- 安装前后校验四个正式 ONNX 模型的固定大小与 SHA-256，防止错误模型进入运行环境\n\n" +
		"修复与优化：\n" +
		"- 录入活体与实际保存的人脸帧绑定，阻止通过独立调用或中途换照片绕过\n" +
		"- 账号身份切换后可刷新已保存信息，人脸数据无需重新录入\n" +
		"- 暗光增强仅用于人脸识别，活体检测保持标定时的原始预处理\n\n" +
		"说明：\n" +
		"- 现有人脸数据兼容；旧活体阈值会自动迁移到双模型标定值"

	// Initialize the embedded resource filesystem in the internal package
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
