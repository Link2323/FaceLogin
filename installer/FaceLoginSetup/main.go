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
	internal.NoticeVersion = "1.6.0-multi-angle"
	internal.NoticeTitle = "FaceLogin 1.6.0-multi-angle 更新说明（Link2323 fork）"
	internal.NoticeBody = "功能：\n" +
		"- 多角度人脸录入：正面、左转 30°、右转 30° 三角度，侧脸识别更稳定\n" +
		"- 匹配阈值滑块生效：基于真实标定数据放开，限定在安全区间，可按需调节严格度\n" +
		"- 采用 SCRFD 自带 5 关键点对齐，不再依赖 dlib，精度无损\n\n" +
		"性能：\n" +
		"- 模型 INT8 量化 + 安装包体积大幅缩减\n" +
		"- 混合架构 CPU 自动绑定 P 核，低端笔记本识别速度显著提升\n" +
		"- 重型模型后台懒加载，冷启动更快\n\n" +
		"安全：\n" +
		"- 注册表数据目录重定向防护（运行时路径校验 + 注册表权限锁定）\n" +
		"- 密码内存清零加固，认证失败路径不再残留明文\n" +
		"- 运行时模型完整性校验，阻止替换活体模型绕过防御\n" +
		"- 活体模型被篡改/损坏时，锁屏显示明确的完整性校验失败提示（此前仅在日志中可见）\n" +
		"- 安装器引导选择受系统级 ACL 保护的安装目录，防止程序文件被篡改\n\n" +
		"修复：\n" +
		"- 修复锁屏空场景误报「检测到攻击」，失败提示文案更准确\n" +
		"- 修复账号身份切换误报、录入撞槽位等问题\n\n" +
		"说明：\n" +
		"- 现有人脸数据兼容，无需重新录入（多角度为新增可选能力）"

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
