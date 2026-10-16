# FaceLogin Setup

FaceLogin Setup 是 Go/Wails 安装与卸载程序，不参与运行时认证。

- 完整构建、资源同步与打包流程：[`docs/BUILD.md`](../../docs/BUILD.md)
- 安装器职责、生命周期和安全边界：[`docs/modules/installer.md`](../../docs/modules/installer.md)
- 前端维护说明：[`frontend/README.md`](frontend/README.md)

`resources/` 是构建载荷镜像，不是资源清单的事实源；`frontend/wailsjs/` 是生成绑定，不要手工编辑。
