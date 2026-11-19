# Pulse

**为 Windows 打造的文件管理器，让浏览、搜索和整理文件更顺手。**

Pulse 将多窗口、多标签、多窗格、文件与正文搜索、预览和批量文件操作放在同一个工作空间。

本仓库是 Pulse 的独立修改发行版，由 EternalLightning 维护。原项目由 jimmgreen 及其贡献者开发。[上游仓库](https://github.com/jimmgreen/pulse) · [版本差异](#与上游-pulse-的区别) · [来源与许可证](#来源与许可证)

[下载与历史版本](https://github.com/EternalLightning/pulse/releases) · [反馈问题](https://github.com/EternalLightning/pulse/issues)

![Pulse 外观](docs/images/appearance.png)

## 与上游 Pulse 的区别

比较基于本仓库 `1.5.5.0` 与上游 `main` 的 `b324368`。两个仓库独立编号。[上游 README](https://github.com/jimmgreen/pulse/blob/b32436831a066e18448b2810bef4bf989024fc68/README.md)

| 功能 | 本发行版 | 上游 Pulse |
| --- | --- | --- |
| 系统与更新 | 面向 Windows 10 / 11 x64，提供安装包和免安装 ZIP，从本仓库 Releases 手动下载更新。 | 另提供 Windows 8.1 x64 兼容安装包，支持在应用内检查、下载和安装更新。 |
| 正文搜索与压缩包 | 随包提供文档提取程序、PDFium 和 7-Zip。可搜索受支持文件的正文，也可浏览和解压 ZIP、7z、RAR。 | 正文搜索使用按需增强包。内置 ZIP 预览，7z、RAR 需要另行安装并启用原生压缩包插件。 |
| 会话恢复 | 启动时保留窗口偏好，导航从新会话开始。 | 自动更新后可恢复保存的窗口会话。 |
| 首页、别名与工具栏 | 首页卡片可独立配置，有 Windows 库、控制面板和应用程序入口。用 `@work` 形式的文件夹路径别名跳转，窗格标题栏单独提供查看菜单。 | 文件和文件夹均可设置显示别名，支持集中管理和导入导出。工具栏可固定命令或程序并加入分隔线。 |
| 文件选择对话框 | Shell 桥接与前台兼容接管可分别启用。兼容接管默认关闭，打开窗口时可按住 Shift 跳过。 | 提供实验性的打开与保存对话框接管，场景不受支持时保留 Windows 原生界面。 |
| 助手与扩展 | — | 提供 Pulse 助手和开发预览阶段的原生插件 SDK，外部 MCP 客户端可通过单独的 AI 增强包连接。 |

其实本项目与上游项目最大的区别应该在 UI 部分，本项目大量的 UI 借鉴了 Files 的思路，优化了一些过渡动画，增加了一些定制功能。

## 让工作空间适合你

- 浅色、深色或跟随系统主题，标题栏一键切换；可选主题色、窗口效果（亚克力、Mica）、壁纸背景、界面透明度与背景模糊。
- 详细信息视图支持智能日期（「今天 14:32」「昨天 09:10」）、斑马纹行、彩色扩展名标签、大小比例条，列宽按内容自适应；右键列标题可选择显示哪些列。
- 七种颜色标签可以给文件和文件夹做标记，侧栏按标签汇总；也可以直接在资源管理器右键菜单中打标签。
- 侧栏分区可拖动排序、折叠，标签页可放到侧栏纵向排列（Ctrl+B 收起 / 展开）。

## 自定义常用操作

右键工具栏按钮或空白处，选择「自定义工具栏…」，在独立窗口中添加、移除、拖动排序或恢复默认按钮。顶部「更多」只收纳窄窗口放不下的按钮；没有溢出时，该位置直接打开自定义窗口。文件显示模式集中在窗格标题栏的「查看」菜单。

普通命令的快捷键在「设置 → 快捷键」中搜索、修改、解绑或恢复默认，与工具栏配置分开。

首页「快捷访问」标题右侧的「自定义」或「设置 → 快捷访问」可调整首页卡片：「库文件夹」包括桌面、文档、图片、音乐、视频、下载，均可切换显示；回收站及「库」「控制面板」「应用程序」也可分别开关。通过「添加文件夹…」选择自定义文件夹，并可移除或上下调整顺序。配置自动保存并同步到其他窗口，与侧栏固定位置分别管理。

详细信息视图右键列标题可配置列顺序与显隐，新增尺寸、时长和拍摄时间列；列配置随文件夹保存。媒体信息在后台读取，文件未提供的属性留空。

命令面板中的「路径别名」可管理常用目录，例如把项目目录命名为 `work`，在地址栏输入 `@work` 或 `@work\子目录` 即可跳转。首页与命令面板还提供「库」「控制面板」「应用程序」入口；库会汇总其成员文件夹中的文件，控制面板和应用程序项目支持通过 Windows 打开及查看属性。

## 更快找到文件

- 通过 PulseIndex 索引搜索整盘文件名，结果实时更新；可以限定在当前文件夹或指定文件夹中搜索。
- 文件名搜索支持全拼、首字母和多音字，例如输入 `sh` 即可找到「品牌设计」「城市夜景」「山湖晨雾」，命中的文字会高亮。
- 内容搜索覆盖文本与代码、PDF、Word、Excel、PowerPoint 等格式，结果流式加载，打开所在位置后可后退回到原结果。
- 高级搜索（Ctrl+Shift+F）按类型、日期、大小等条件筛选；全局快速搜索（Alt+Space）随时呼出，也可以「在主界面搜索」查看全部结果。
- 地址栏输入 `cmd`、`powershell`、`pwsh` 或 `wt`，直接在当前文件夹打开终端。

![Pulse 拼音搜索](docs/images/search.png)

## 多个目录，一起处理

- 单栏、左右分屏、上下分屏、三栏和四宫格布局（Ctrl+1/2/3/4），每个标签页保留自己的布局和位置。
- 分栏浏览同时展示上级、当前和选中的文件夹；平铺、图标、列表、详细信息和内容视图随时切换。
- 列表可按名称、日期、类型或大小分组，每个文件夹分别记住视图、排序和分组。
- 双栏文件夹对比标出两侧不同、缺失和较新的项目；后台统计文件夹大小。
- 详情面板显示预览、基本信息和标签；多选后可打开合并属性窗口。

![Pulse 多窗格与分栏浏览](docs/images/panes.png)

## 空格快速预览

选中文件按空格即可预览，无需打开对应应用：

- Markdown 按排版显示，代码语法高亮，PDF 逐页翻看，文件夹显示内容列表。
- CSV / TSV / XLSX 以表格显示；JSON / XML 显示为可折叠的树，Jupyter Notebook 按单元格显示。
- DOCX、EPUB 和 Office 文档无需安装对应软件即可阅读；压缩包显示目录树并可在包内搜索。
- 视频可播放、拖动和逐帧，动态 WebP / APNG 可播放；另支持 SVG、PSD、AI、DWG 缩略图和字体等格式。

「设置 → 快速预览 · 支持的格式」列出全部可预览格式，并检测 HEIF / HEVC / AV1 / WebP 系统扩展是否已安装。

## 暂存盘与文本对比

复制或剪切（Ctrl+C / Ctrl+X）的文件会收进左下角的暂存盘，可以跨文件夹、跨标签页收集后一次拖到目标位置；拖放默认复制，按住 Shift 为移动。

暂存两个文件时可以对照位置、修改时间、大小和内容；文本文件点击「查看差异」打开对比窗口，支持并排 / 统一视图、行内差异高亮、仅看差异、忽略空白和大小写，以及 F7 / Shift+F7 跳转。

## 融入 Windows

- 可以在设置中把 Pulse 设为默认文件管理器，用于打开文件夹、Win+E 和桌面上的「此电脑」；「打开文件所在位置」会在 Pulse 中选中文件。随时可以关闭，恢复原样。
- 在「设置 → 启动与集成」可分别打开文件选择窗口的 Shell 桥接和兼容接管。兼容接管只识别前台的标准 Windows 对话框，默认关闭。打开时按住 Shift 可跳过，也可在选择器中切回 Windows 对话框。
- 文件列表获得焦点时，按 A–Z 选中名称以对应字母开头的第一项，继续按同一个字母会按当前显示顺序循环。
- 右键菜单合并资源管理器与 Windows 11 新式菜单项（如 VS Code、Windows Terminal、NanaZip），Pulse 自带项可以隐藏。
- 支持开机启动到托盘、设置默认位置，以及保存和手动恢复具名工作区。
- 文件被占用时显示占用进程，可以结束进程后重试；「此电脑」显示磁盘类型和容量条。

![Pulse 设置与系统集成](docs/images/settings.png)

## 从源码构建

使用 Windows 10 / 11 x64、Visual Studio 2022 v143 C++ 工具、Windows SDK 和 Python 3.10+。构建入口会下载并校验 `scripts/build-tools.lock.json` 固定的 CMake 和 Ninja，缓存于 `.build-tools`。便携包、产物校验与 CI 脚本另需 PowerShell 7.2+（`pwsh`）。安装包需要 Inno Setup 6。

在仓库根目录运行；脚本会自动查找并初始化 x64 Visual Studio 工具环境：

```powershell
.\build_release.bat
.\build-release\Pulse.exe
```

默认使用 Ninja 目录 `build-release`，构建六个交付程序：`Pulse.exe`、`Pulse.Index.exe`、`Pulse.Document.exe`、`Pulse.Preview.exe`、`Pulse.Shell.exe` 和 `Pulse.Integration.exe`。发布构建选择静态 MSVC 运行库（`/MT`）、关闭内嵌自测（`PULSE_WITH_SELFTEST=OFF`），仅构建交付目标及其依赖，不运行测试。

日常修改后，使用 `.\scripts\build.ps1 -BuildDir build-release -Targets pulse` 只构建受影响目标；脚本负责初始化工具链并持有构建目录锁。`pulse` 会带上索引、Shell、预览、文档提取和集成程序；仅修改某个辅助模块时也可单独构建对应目标。

删除并重新配置构建目录可用 `.\build_release.bat /clean`，执行前请保存仍需保留的构建产物。普通构建不会自动转换已有的其他生成器缓存。自定义目录、LumaText SDK 和定向测试见[构建与打包指南](docs/build-and-package.md)。

## 打包安装包与免安装包

在仓库根目录运行以下流程；每一步成功后再执行下一步：

```powershell
.\build_installer.bat
pwsh -NoProfile -File .\scripts\package_portable.ps1 -BuildDir build-release
```

两个产物的版本号均读取自 `version.txt`：

- 安装包：`dist\PulseSetup-<version>.exe`。
- 便携包：`dist\Pulse-<version>-portable-win-x64.zip`。

安装脚本先构建 Release，再检查六个程序的版本和原始文件名、内嵌自测标记及运行库依赖，最后编译安装包。便携包脚本直接使用现有构建文件，不负责构建。两个包均包含辅助程序、LumaText、PDFium、7-Zip 与第三方许可证；重复打包会覆盖同版本同名文件，需要保留旧包时先自行备份。

若刚完成同一份源码的 Release 构建，可以用 `.\build_installer.bat /skipbuild build-release` 跳过重复构建。`/skipbuild` 和便携包脚本都不检查源码是否已变更，也不运行功能测试。

生成后可核对便携包全部文件，并记录安装包的大小和 SHA-256：

```powershell
$version = (Get-Content .\version.txt -Raw).Trim()
pwsh -NoProfile -File .\scripts\verify_release_artifacts.ps1 `
    -BuildDir build-release `
    -ZipPath "dist/Pulse-$version-portable-win-x64.zip" `
    -InstallerPath "dist/PulseSetup-$version.exe" `
    -OutputJson "docs/validation/packaging-$(Get-Date -Format yyyyMMdd-HHmmss)/artifacts.json"
```

此校验不会解包核对安装器载荷或执行安装，也不会自动生成 `.sha256` 文件。安装包运行时需要管理员权限，可选择安装 PulseIndex 后台索引服务；便携包解压完整目录后运行 `Pulse.exe`，不会注册服务，配置、缓存和日志仍保存在用户应用数据目录。

完整命令、运行库检查、备份和校验文件生成见[构建与打包指南](docs/build-and-package.md)。生成包、替换现有安装和发布 GitHub Release 分别执行；当前打包命令不会更新已安装程序或上传产物。

## CI 与正式发布

推送与 `version.txt` 匹配的标签即可触发 GitHub Actions 发布，例如 `v1.5.5.1`。工作流运行回归测试，从同一次生产构建生成安装包和便携 ZIP，校验后发布。签名通过独立的 `scripts/package_release.ps1` 执行，默认使用 `build-release`。[发布工作流](.github/workflows/release.yml) · [发布与签名步骤](scripts/RELEASE.md)

源码构建和发行包使用固定的 LumaText SDK。运行库与 SDK 版本的维护方法见[运行库与 SDK 文档](docs/static-runtime-packaging.md)。

技术栈为 C++20、Win32、Direct2D 和 DirectComposition。文件系统、索引、预览与 Shell 任务分别放在对应模块，避免阻塞界面。

更多说明：[索引存储与迁移](docs/index-migration.md)

界面开发：[UI 与文字渲染规范](docs/ui-style-guide.md) · [LumaText 覆盖与迁移建议](docs/lumatext-coverage.md)

## 来源与许可证

Pulse 的上游代码与本发行版的项目代码、脚本和文档采用 **Apache License 2.0**。第三方代码、图标和运行库适用各自的条款。下表列出代码改编和界面素材的主要来源。[Apache-2.0 全文](LICENSE) · [发行版归属与修改声明](NOTICE)

| 原项目或素材 | 在本发行版中的用法 | 许可证与来源 |
| --- | --- | --- |
| jimmgreen/pulse | 文件管理器的上游代码。文件对话框 Shell 桥接、选择器 IPC 和相关验证逻辑也从上游适配。 | Apache-2.0 · [适配时的上游提交](https://github.com/jimmgreen/pulse/tree/b32436831a066e18448b2810bef4bf989024fc68) |
| chenzai666/pulse | 适配原生文件对话框兼容接管模块。 | Apache-2.0 · [来源提交](https://github.com/chenzai666/pulse/tree/fc6dae4a2737c2bc3014972fb24d30219bd7624f) |
| Files Community / Files | 应用图标和部分工具栏、窗格与视图图标。工具栏图标从 XAML 转为 SVG，部分图标调整了填色以适应主题。 | 这些素材采用 MIT · [应用图标来源](third_party/files-icon/README.md) · [工具栏图标来源与许可](THIRD_PARTY_NOTICES.md#files-工具栏图标来源) |
| wangwenx190/d2d-mica | 适配 Direct2D 材质图，用于自定义背景取样、着色和 Mica 效果。 | MIT · [原项目](https://github.com/wangwenx190/d2d-mica) · [适配说明及版权](third_party/d2d-mica/README.pulse.md) |
| Microsoft Fluent System Icons | 首页图标和部分工具栏图标。 | MIT · [对应 SVG 的来源](assets/fluent/SOURCES.md) · [许可证](assets/fluent/LICENSE) |
| Microsoft OneDrive 产品图标 | 在检测到的 OneDrive 同步目录旁标识该服务。 | Microsoft Fabric Assets License Agreement · [原始素材及使用范围](assets/microsoft_brand/README.md) · [协议全文](assets/microsoft_brand/LICENSE.pdf) |

拼音数据、SQLite、LumaText、PDFium 和 7-Zip 的来源、许可证及包内位置见[第三方声明](THIRD_PARTY_NOTICES.md)。安装包与免安装 ZIP 均附带项目许可证、归属声明和第三方许可文件。随附的 7-Zip 还带有对应版本的上游源码归档。[7-Zip 版本与源码说明](third_party/7zip/README.md)

### 再分发

Apache-2.0 允许在遵守条款的前提下使用、修改和分发本项目，包括商业使用。分发源码或二进制时，应向接收者提供完整的 `LICENSE`，保留适用的版权、专利、商标及归属声明，并随附 `NOTICE` 中的适用归属信息；修改过的文件应带有醒目的修改声明。仅保留 Git 提交记录不能替代文件内的修改声明。

本发行版在项目源码文件中添加许可标识和修改声明；原有第三方声明仍然保留。后续修改请继续遵守上述要求。提交并有意纳入本项目的贡献，除另有明确约定外，按 Apache-2.0 第 5 条提供；贡献者应确保有权提交相关内容。

第三方组件及素材的许可证随仓库保存，并由打包脚本随发行文件附带。再分发时应同时检查[第三方声明](THIRD_PARTY_NOTICES.md)，尤其是 7-Zip 的 LGPL / unRAR 条款和 Microsoft 产品图标的专门使用条款。Apache-2.0 不授予项目或第三方的商标使用权。软件按原样提供，不作保证；完整权利、条件及责任限制以各许可证原文为准。
