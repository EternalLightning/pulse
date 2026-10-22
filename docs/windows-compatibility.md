# Windows 兼容性

Pulse 仅支持 Windows 10 / Windows 11 x64，安装器最低系统版本为 Windows 10。

窗口材质、DPI和媒体组件按实际可用接口启用，保留 Windows 10 不同版本及 Windows 10/11 之间所需的功能检测。

发布构建使用 Visual Studio 2022 v143 和静态 VC 运行库。运行 `pwsh ./scripts/build_release_ci.ps1` 可完成发布构建、定向回归检查和安装包生成。
