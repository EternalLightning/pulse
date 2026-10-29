# 虚拟页面导航与错误空页闪烁排查

日期：2026-10-25。范围：地址栏导航身份、同类虚拟页面、错误目录后台刷新及空状态插图。保留本次开始前和并发任务的其他修改。

## 导航问题

`FillPaneViewModel` 原先用 `virtual_title` 或 `DisplayPath(current_path)` 填写 `PaneViewModel.path`；这个字段随后被 `SplitBreadcrumb`、命中测试和 `NavigateTo` 当作地址使用。此电脑的真实地址为空字符串，而 `DisplayPath` 将它替换为“此电脑”。点击后 `NormalizePath` 把该名称解析为程序工作目录下的相对路径，与用户截图的 `D:\Applications\Pulse\此电脑` 一致。

此前仅对主页、压缩包、查询搜索、回收站保留了地址，其他页面仍可能把显示名称送入文件系统解析。新增用例在修复前得到 **90 PASS / 48 FAIL**，失败覆盖以下八类页面的两种语言与三项地址/导航断言：

| 页面 | 导航身份 | 旧实现的问题 |
| --- | --- | --- |
| 此电脑 | 空字符串 | 本地化名称被当成相对路径 |
| 收藏 | `pulse:starred` | 页面标题覆盖地址 |
| 最近使用 | `pulse:recent` | 页面标题覆盖地址 |
| 标签 | `pulse:tag:<id>` | 用户定义的标签名称覆盖标识 |
| 保存的搜索 | `pulse:saved-search:<id>` | 搜索名称或状态文字覆盖地址 |
| 设置 | `pulse:settings:<page>` | 页面标题覆盖地址；当前设置界面没有普通面包屑栏 |
| 工作区 | `pulse:workspace:<index>` | 显示名称可能覆盖动作地址 |
| 更改视图 | `pulse:changes:<id>` | 页面标题覆盖地址 |

统一修复：`PaneViewModel.path` 始终保留可导航地址；`header_text` 单独携带显示名称。普通路径保留现有可读显示形式，虚拟地址完整保留原字符串。面包屑布局显式接收显示标题，其点击目标不受标题影响。移除针对个别虚拟页面的路径补丁。

同类边界一并处理：此电脑地址编辑器用提示文字呈现名称，实际值保持空地址；直接提交不会跳到工作目录或系统盘。后退历史保留合法的空地址，因此离开此电脑后可以后退并前进。真实目录即使命名为“此电脑”或“Home”，仍按其实际地址导航，不按显示名称猜测虚拟页面。

入口核查：面包屑布局、命中测试、单击分发、面包屑菜单、原生地址编辑器、侧栏路径、标签页和会话恢复。面包屑菜单沿用命中测试的地址，侧栏项与会话持有实际路径；当前检查未发现其他把 `virtual_title`、`header_text` 或侧栏 `label` 交给 `NavigateTo` / `NewTab` / `NormalizePath` 的生产入口。这是当前代码检查结论，不代表已经人工走完所有系统集成场景。

## 闪烁问题

`DrainDirNotifies` 每秒轮询没有成功建立目录监听、且没有刷新请求在途的可见路径。不存在或不可访问的目录无法建立监听，因此会持续进入 `RefreshPath`。旧逻辑仅以“没有快照”判断 `loading=true`；第一次错误已经显示后，每次后台重试仍重新进入加载态，而 `DrawSinglePane` 在加载期间不绘制空状态。于是插图、说明和按钮反复消失、重现。

真实缺失目录夹具在修复前得到 **83 checks / 7 failures**：三次自动重试都重现了状态切换与插图像素变化，操作完成后的后台刷新同样重现。与此对照，五类静态空状态 × 浅深色 × 100%/150% 缩放，每组连续 32 帧，共 **640 帧**，插图区域像素完全相同。此次证据指向错误重试的加载状态，不支持修改 SVG 绘制或增加图标动画补丁。

修复限定在刷新状态判定：没有快照时，仅首次加载（尚无已完成的结果）或显式刷新进入加载态；已显示错误的后台重试保留当前画面。后台轮询和真实目录枚举继续运行，目录恢复后仍需发布正常列表、清除旧错误。显式刷新仍显示加载状态。

截图证据：[错误已显示时](before/missing-settled.png)、[修复前后台重试时](before/missing-retrying.png)、[修复后错误已显示时](after/missing-settled.png)、[修复后后台重试时](after/missing-retrying.png)。修复后两张截图的 SHA-256 完全相同：`E1C916BE5718CC42F01B38AB0D1310029F7194EADA85B7656B3CD61661BB73CF`。

## 定向验证

| 检查 | 结果 |
| --- | --- |
| `pulse_breadcrumb_test` | [164 PASS / 0 FAIL](after/breadcrumb.log) |
| `pulse_virtual_location_navigation_test` | [427 checks / 0 failures](after/navigation-ui.log) |
| `pulse_empty_state_render_test` | [83 checks / 0 failures](after/empty-state-render.log) |
| Release 主程序目标 `pulse` | [构建通过，退出码 0](after/pulse-build.log) |

模型检查包括此电脑、主页、收藏、最近、回收站、标签、查询搜索、保存的搜索、设置、工作区、更改视图、压缩包，以及普通目录、UNC、长路径和历史恢复。集成检查已验证实际鼠标处理、原生地址编辑器、中英文、720/1100 DIP、100%/150% 缩放；设置界面没有普通面包屑栏，单独检查其模型/布局地址与原生编辑器，不冒充实际面包屑点击验证。

空状态检查使用隐藏的生产 compositor 与真实目录枚举 worker，验证了 640 帧静态渲染、三次实际后台轮询、显式刷新、操作完成触发的刷新，以及新建此前缺失的目录和文件后自动恢复正常列表、清除旧错误。隔离夹具文件已清理。三组检查合计 **674 项通过、0 失败**；未因本次修复运行全部历史测试。

本次构建目录为 `build_audit_msbuild_20261025`，使用 Release 配置。只请求三个相关测试目标和主程序目标，主程序命令为 `cmake --build build_audit_msbuild_20261025 --config Release --target pulse -- /m:1 /nr:false /nologo /v:minimal`。受影响文件的 `git diff --check` 通过。

Windows 执行环境：构建输出继承 Low Integrity 标签，直接运行两个 GUI 夹具时在进入 `main` 前退出 `0xC0000142`。两个夹具复制到已授权可写、无 Low 标签的隔离目录，与原文件及所需相邻 `lumatext.dll` 做 SHA-256 一致性核对，再以普通用户权限运行通过；未提权，未修改 ACL。导航夹具哈希记录见 [navigation-ui-hashes.txt](after/navigation-ui-hashes.txt)。

验证日志保存在本目录 `before/` 与 `after/`。最初代码修复验证阶段未替换用户安装目录，隔离夹具与构建结果不能视为安装版验收；用户随后授权的安装替换及检查结果见下节。

## 后续更名及安装替换

用户要求将管理员辅助程序正式命名为 `Pulse.Integration.exe`。已同步 CMake 输出名、PE `OriginalFilename`、主程序调用路径及报错文字、便携包、安装包文件清单、运行库扫描、发布检查和符号归档文件名。逻辑构建目标仍为 `pulse_integration`。旧名称仅保留在安装迁移的清理规则中。

安装器按精确名称清理旧文件；直接替换脚本将旧 `pulse_integration.exe` 纳入同一次备份、预检、哈希验证和失败回滚事务。迁移夹具同时发现并修复了无差异计划在 StrictMode 下读取空数组成员的问题。

本次更名验证：

- [Release 主程序及辅助程序构建通过](after/integration-rename-build.log)。
- 新程序的 `OriginalFilename` 为 `Pulse.Integration.exe`，主程序二进制包含新调用路径；[普通权限、相同哈希副本的启动检查](after/integration-helper-startup.log)按预期以参数错误码 2 退出，没有执行注册表更改。当前 Release 配置不生成 PDB；未运行符号归档。
- [迁移及失败回滚夹具](verify-integration-replacement.ps1)：[12 checks / 0 failures](after/integration-replacement-fixture.log)。安装目录全部为工作区下的临时目录，服务/进程状态为夹具模拟；此检查不代表已验证真实安装服务状态。夹具已清理。
- 六个发布 EXE 的版本和原始文件名检查通过；新 ZIP 包含 `Pulse.Integration.exe`，不包含旧名称。七个相关 PowerShell 脚本语法检查及本次修改的差异空白检查通过。没有重跑此前的导航和渲染测试。

准备好的新载荷目录：`E:\Projects\pulse\dist\portable-stage-ad9dc8e2cef5475389c3a4ea18d78923\Pulse-1.2.1.1-portable`。与当前安装目录的只读哈希比较共 41 个载荷文件，加一项旧文件退役，计划涉及 6 项变化：[清单](after/installed-replacement-plan.csv)。该清单是只读比较，正式替换前仍由脚本重新预检。

准备时安装版 Pulse UI 仍驻留，用户随后确认退出，已通过当前进程检查确认。权限受限期间，真实进程/服务查询仍返回拒绝访问：[退出后的预检结果](after/integration-installed-plan-after-exit.log)；当时没有写入安装目录。用户随后恢复完整访问权限并要求继续，真实预检通过。

**安装替换现已完成。** 后台网络索引代理占用了变化的 `Pulse.Index.exe`，因此在替换前停止该代理，完成替换后以普通用户权限恢复；没有强制终止 Pulse UI，没有改变索引服务的 Stopped / Auto 状态。执行脚本时代理不在运行，脚本验证这一事务内的运行状态不变；事务前后的代理角色另行恢复、核验并记录，而非要求 PID 不变。

- [替换结果](after/integration-installed-apply.log)：Success，6 项变化，42 项验证（41 个载荷文件与旧文件退役）。
- 回滚目录：`E:\Projects\pulse\dist\installed-backups\20261025-175738-fe09a6a163e34387bad72f827732084d`。原安装中的 41 个文件全部备份并核对原 SHA-256；安装后的 41 个载荷文件全部匹配目标 SHA-256，旧 `pulse_integration.exe` 已不存在。
- [安装后独立复核](after/integration-installed-verification.json)：再次验证全部安装和备份哈希；[主页](after/installed-home.png)、[错误目录](after/installed-missing.png)使用实际安装路径的 `Pulse.exe` 完成隐藏窗口截图，退出码均为 0，已检查画面。安装目录的新名称辅助程序正常启动，并按预期以缺少参数的错误码 2 退出，没有执行注册表修改。
- [运行状态前](after/integration-runtime-before.json)与[运行状态后](after/integration-runtime-after.json)：网络索引代理从 PID 49032 恢复为 56604；服务仍为 Stopped / Auto，路径和参数不变。
- 最后以普通用户权限重新打开安装版，PID 7712，已确认可见主窗口标题为“主页 - Pulse”。没有修改用户配置文件来完成测试。

安装版检查覆盖启动、页面呈现和新辅助程序可启动性；没有在安装版中重新执行全部交互回归，也没有将静态截图视为后台重试闪烁的动态验证。该动态边界的实际 worker 与像素序列证据仍为前述定向夹具。
