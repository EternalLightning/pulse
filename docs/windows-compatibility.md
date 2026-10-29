# Windows 兼容性

Pulse 仅支持 Windows 10 / Windows 11 x64，安装器最低系统版本为 Windows 10。

窗口材质、DPI 和媒体组件按实际可用接口启用，保留 Windows 10 不同版本及 Windows 10/11 之间所需的功能检测。支持范围不等于本次已完成这些系统上的实机验证，具体结果见[验证记录](validation/README.md)。

本地生产构建使用 MSVC x64、Windows SDK、CMake 3.25+ 和 Ninja，`build_release.bat` 会自动初始化可用的 Visual Studio C++ 工具链。CI 和固定的 LumaText SDK 另限定为 Visual Studio 2022 v143。生产使用静态 MSVC 运行库（`/MT`），但仍需随包交付 LumaText、PDFium 和 7-Zip 的文件及许可证。

当前本地安装包入口为 `build_installer.bat`，便携包由 PowerShell 7.2+ 运行 `scripts/package_portable.ps1 -BuildDir build-release` 单独生成。安装器使用 Inno Setup 6，运行时需要管理员权限，并可选择注册 PulseIndex 服务。便携包解压不注册服务，配置和缓存仍在用户应用数据目录。

命令和产物校验见[构建与打包指南](build-and-package.md)，运行库扫描和 SDK 固定见[静态运行库说明](static-runtime-packaging.md)。CI 的验证范围及发布前置条件见[发布流程](../scripts/RELEASE.md)，工作流中的便携 ZIP 需要单独准备并上传到对应草稿。
