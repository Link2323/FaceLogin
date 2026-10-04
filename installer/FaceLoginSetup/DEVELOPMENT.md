# FaceLogin Setup 开发入口

安装器的完整构建命令、资源清单和打包流程统一维护在 [`docs/BUILD.md`](../../docs/BUILD.md)。安装/卸载顺序、ACL 和载荷生命周期见 [`docs/modules/installer.md`](../../docs/modules/installer.md)。本文件只保留本目录的开发入口：

- Go 后端：`app.go`、`internal/`
- Vue 前端：`frontend/src/`
- 安装器测试：在本目录运行 `go test ./...`
- 前端静态检查与构建：在 `frontend/` 运行 `npm run build`
- `frontend/wailsjs/` 与 `resources/` 均为生成或同步产物；改动方法或载荷时按上述构建文档重新生成/同步，不直接维护生成文件。
