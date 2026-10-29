# 静态运行库与 LumaText SDK

常用构建和打包步骤见[构建与打包指南](build-and-package.md)。本页说明当前运行库检查、SDK 固定及更新边界。

## 生产运行库配置

Pulse CMake 默认选择静态 MSVC 运行库；`build_release.bat` 显式设置 `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`（Release `/MT`），并关闭内嵌自测。CMake 直接配置时仍尊重显式的运行库选择，因此不能仅凭目录名判断交付文件使用了 `/MT`。

LumaText 保持独立 DLL 和原有 C ABI，PDFium 及 7-Zip 也仍有需要随包交付的文件。静态 MSVC 运行库不表示只交付一个 EXE。

本地安装批处理在调用 Inno Setup 前运行：

```powershell
pwsh -NoProfile -File .\scripts\stage_installer_runtime.ps1 `
    -BuildDir build-release -RequireStaticRuntime
```

脚本用 Visual Studio 的 `dumpbin` 检查六个生产 EXE、`lumatext.dll` 和 `pdfium.dll` 的普通及延迟导入，并追踪发现的 CRT 依赖。若仍需 MSVC 动态运行库，`-RequireStaticRuntime` 会拒绝继续。它生成 `installer-runtime-manifest.json`；静态交付应无所需 CRT DLL。

`package_portable.ps1` 和 `verify_release_artifacts.ps1` 不运行这项扫描，单独生成 ZIP 时应对实际构建目录另行检查。`build_release_ci.ps1` 目前也未调用这项扫描，其现有缺口见[发布流程](../scripts/RELEASE.md)。

脚本不带 `-RequireStaticRuntime` 时可暂存开发用动态运行库；安装脚本中的 `AppLocalRuntime` 条件也保留供显式开发使用。本地生产入口不会自动启用该模式，便携脚本也不会自动收集这些暂存 DLL。

旧 `/MD` 产物需要先重建并确认依赖后再移除运行库。静态安装包升级时只删除 `{app}` 内四个明确列出的旧文件：`msvcp140.dll`、`msvcp140_atomic_wait.dll`、`vcruntime140.dll`、`vcruntime140_1.dll`，不修改 Windows 或共享 VC 运行库。

## 固定的预编译 SDK

本地 Release 脚本默认使用仓库内 `third_party/lumatext`，无需每次重建 LumaText、HarfBuzz 和 FreeType。CI 先调用：

```powershell
pwsh -NoProfile -File .\scripts\verify_lumatext_sdk.ps1
```

`cmake/lumatext-sdk.json` 指定 `third_party/lumatext/sdk-manifest.json` 并固定该清单的 SHA-256。校验器要求清单为 schema 1、x64、Release、MT、v143，逐项核对文件大小和哈希，检查必需项，并拒绝 SDK 目录内未列入清单的额外文件。

清单覆盖 DLL、导入库、C/C++ 头文件、CMake 导出和许可证等实际 SDK 文件。当前清单与二进制可能随 SDK 更新而变更；以仓库当前清单和 pin 为准，不能把早期的 `local-sdk-manifest.json` 或另一份 SDK 输出目录当作 CI 的固定来源。本地 Release 脚本不自动执行该完整校验，需要时可单独运行。

## 源码开发与 SDK 更新

显式设置环境变量 `LUMATEXT_SOURCE_DIR` 可让本地 Release 脚本改用 LumaText 源码；该目录必须包含其 CMakeLists.txt。Pulse 配置会关闭 LumaText 的测试、样例和参考工具，并把运行库配置传给源码目标。未显式设置、仓库预编译 DLL 又不存在时，脚本才会尝试 `../lumatext`。

源码目录中 `build-vs18/_deps/harfbuzz-src` 和 `freetype-src` 是该脚本可复用的本地依赖缓存，不是干净检出的前置目录；其他源码构建依赖由对应 LumaText 项目负责。使用源码开发不会自动更新仓库内预编译 SDK 或 CI pin。

更新预编译 SDK 时，应从同一份已验证的 x64 Release `/MT`、v143 构建同步 DLL、导入库、头文件、CMake 导出和许可证，生成覆盖实际文件的 `sdk-manifest.json`，再更新 `cmake/lumatext-sdk.json` 中的清单哈希并运行校验器。源码版本、快照说明和验证结果也应与本次 SDK 相符。不要只替换 DLL 或手改哈希来掩盖混用的 SDK 文件。

LumaText 自身的构建选项、测试目标和 SDK 导出方式由对应源码版本维护；早期依赖本机 `build-vs18` 缓存的完整重建命令不再作为本仓库通用步骤。只有更新 SDK 或修改 LumaText 时才需要扩大到相关 ABI 和渲染验证。

静态 CRT 内存应在分配它的模块内释放。Pulse 使用 LumaText 不透明句柄和 `lt_release`，以及 POD 描述符和 COM 引用作为模块边界，不应让调用方直接释放 DLL 内分配的内存。

## 原 CRT 迁移的历史验证

以下保留原文档中的迁移结果，属于当时的构建，不是本次文档更新或当前 SDK 的重新验收：

- 当时检查的七个安装载荷二进制均无 MSVC 动态运行库导入；当前扫描清单已是六个 EXE 加两个 DLL。
- C 头文件、离屏中文渲染和 glyph-provider 检查通过。另以 `/MD` 且启用断言的 ABI 程序检查 `/MT` DLL，通过；普通 Release ABI 目标禁用断言，未计作 ABI 验证。
- 隔离文件上的 Shell 创建、ping 和后续响应检查通过。
- 安装前检查先拒绝旧 MD 载荷，再接受 MT 载荷并生成空运行库清单。
- 在相同自测配置下，嵌入 CRT 使载荷增加 320,272 字节，同时移除 930,032 字节运行库文件；该结果不表示所有版本安装体积都会缩小。
- 当时未覆盖用户正在运行的 Program Files 安装版，也未做 Windows 8.1 实机验证；当前支持范围是 Windows 10 / 11 x64。当时未更新远端 CI SDK pin 的说明不代表当前 pin 仍未更新。

当前打包和安装版替换的实际结果见[验证索引](validation/README.md)，与本页的配置说明分别记录。
