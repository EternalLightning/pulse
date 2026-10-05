# Pulse 本地化资源整理与文案状态耦合审查报告

> 2026-10-05 后续状态：下文保留原审计的时间点和验证记录。“尚未修复”清单已实施后续源码整改，当前实施、定向验证与剩余限制统一见 [资源状态解耦报告](E:/Projects/pulse/docs/resource-decoupling-review.md)；完整功能接口和架构发现见 [功能接口与实现边界审阅](E:/Projects/pulse/docs/architecture-interface-review.md)。不要将下文历史未修复状态或历史测试结果当作当前状态。

## 1. 范围与结论

本报告汇总本次会话完成的资源格式化、界面文案资源化、零散字符串表合并，以及随后对“显示文案被当作状态／错误类型”的静态审查。按要求不包含提交作者、提交归属或其他个人信息。

核心结论：

- **已完成：** 资源 ID 与文本两列左对齐；49 组中英文文案集中管理；原 1530–1946 行的 76 个单条字符串表合并为 4 个分组表。
- **仅调查、尚未修复：** 用错误文字判断取消／权限／冲突，用翻译后的横幅标题判断横幅类别，用显示文字选择颜色／图标，以及英文错误文本参与删除结果可信度和失败路径识别等问题。
- 资源化只解决文案维护位置，不会自动消除“显示文字与机器状态混用”的耦合。后续应先分离机器状态、错误类型和失败路径，再迁移剩余后台显示文案。
- 保留工作区已有修改及用户在任务期间调整的翻译，不把其他并行修改纳入本报告的完成事项。

## 2. 已完成的改动

### 2.1 资源列格式化

[strings.rcinc](<E:/Projects/pulse/src/common/strings.rcinc>) 的资源 ID 统一从第 5 列开始，文本起始引号统一从第 43 列开始，使用空格而非制表符。中英文采用同样的列宽。

首次格式化时逐条检查了原有 1838 条资源文本值，确认只修改行首缩进和 ID 与文本之间的空格，没有修改引号内部的文案、格式占位符、换行转义及有意义的前后空格。

### 2.2 界面硬编码文案资源化

新增 49 个资源 ID，即数值范围 2176–2224，每个 ID 有简体中文和美国英语两种资源；资源文件因此增加 98 条文本记录。

| 分组 | 新增 ID 数 | 覆盖内容 |
|---|---:|---|
| 压缩包 | 16 | 解压／解压全部、密码与解压对话框、目标目录、进度、任务不可用、打开／读取／解压失败及完成提示 |
| 退出 | 8 | 未完成任务确认、停止任务并退出、继续执行、退出中、停止任务中及退出失败 |
| 播放 | 13 | 播放、上一帧／下一帧、逐帧计数、加载／定位／拖动／结束／播放／暂停及播放失败 |
| 链接确认与占用 | 9 | 目标链接标题、继续／全部继续、目录链接／文件链接／硬链接影响、硬链接数量、说明及文件占用消息 |
| 帮助与分隔符 | 3 | 打开操作快捷键、帮助标题上方文字及属性列表分隔符 |
| **合计** | **49** | |

新增资源位于 [中文资源段](<E:/Projects/pulse/src/common/strings.rcinc#L1997-L2049>) 和 [英文资源段](<E:/Projects/pulse/src/common/strings.rcinc#L2051-L2103>)。ID 定义位于 [string_ids.h:958–1006](<E:/Projects/pulse/src/common/string_ids.h#L958-L1006>)，类型安全入口补充至 [localization.h](<E:/Projects/pulse/src/common/localization.h>)；[localization.cpp:13](<E:/Projects/pulse/src/common/localization.cpp#L13>) 的缓存范围上限同步到最后一个新 ID，避免新增文案返回空字符串。

调用点改动：

- [app_commands.cpp](<E:/Projects/pulse/src/app/app_commands.cpp>)、[ui_toolbar.cpp](<E:/Projects/pulse/src/ui/ui_toolbar.cpp>)：解压菜单与工具栏共用资源。
- [archive_dialog.cpp](<E:/Projects/pulse/src/app/archive_dialog.cpp>)、[archive_navigation.cpp](<E:/Projects/pulse/src/app/archive_navigation.cpp>)：移除直接选择中英文的 `Label()` 辅助函数；异常分支原本仅英文的读取／解压失败消息也资源化。
- [app_exit.cpp](<E:/Projects/pulse/src/app/app_exit.cpp>)、[exit_confirmation.h](<E:/Projects/pulse/src/app/exit_confirmation.h>)：退出确认和通知改为读取资源。
- [quick_preview_playback.cpp](<E:/Projects/pulse/src/ui/quick_preview_playback.cpp>)：播放按钮与状态说明读取资源，绘制直接使用资源字符串的数据和长度。
- [file_operation_dialog.cpp](<E:/Projects/pulse/src/ui/file_operation_dialog.cpp>)：目标链接确认和文件占用消息读取资源。
- [shortcut_help.cpp](<E:/Projects/pulse/src/ui/shortcut_help.cpp>)：帮助文案读取资源；固定键名不作为翻译迁移。
- [app_runtime.cpp](<E:/Projects/pulse/src/app/app_runtime.cpp>)、[details_meta.cpp](<E:/Projects/pulse/src/app/details_meta.cpp>)：列表分隔符从资源获取。

实现约束：复用现有“取消”“确定”“暂停”“浏览”等合适资源；浏览按钮保留原有省略号。需要 `const wchar_t*` 的 Win32／COM 调用使用 `.c_str()`，不从临时字符串留下悬空指针。保留 `Create folder: `、`Frame ` 等末尾空格和硬链接数量后缀的开头空格。

### 2.3 定向测试入口

- [localization_test.cpp:20–40](<E:/Projects/pulse/src/bench/localization_test.cpp#L20-L40>) 新增 `--centralized` 筛选，检查新增 49 个 ID 的两种语言资源、代表文案及有意义的前后空格。
- [link_confirmation_ui_test.cpp](<E:/Projects/pulse/src/bench/link_confirmation_ui_test.cpp>) 增加英文参数，保持原有键盘行为检查，并分别保存截图。

## 3. 尚未修复：错误／状态显示文案控制行为

### 3.1 直接比较错误文字

| 编号 | 位置 | 条件 | 后果／风险 |
|---|---|---|---|
| A1 | [app_ops_ui.cpp:289–293](<E:/Projects/pulse/src/app/app_ops_ui.cpp#L289-L293>) | `last_error == L"已取消"` | 在 `Failed` 状态下识别取消，隐藏操作窗口并返回；改成其他语言或另一种取消措辞会不匹配。 |
| A2 | [app_ops_ui.cpp:312–319](<E:/Projects/pulse/src/app/app_ops_ui.cpp#L312-L319>) | 错误包含“没有权限”“拒绝访问”“Access is denied”，或等于 `create failed` | 从文字推断权限问题并改写提示；`create failed` 被笼统归为权限错误，且系统错误文字受 Windows 语言影响。 |
| A3 | [app_ops_ui.cpp:320–321](<E:/Projects/pulse/src/app/app_ops_ui.cpp#L320-L321>) | 错误等于“目标名称已存在” | 推断重命名冲突，选择目标存在提示。 |
| A4 | [app_ops_ui.cpp:322–323](<E:/Projects/pulse/src/app/app_ops_ui.cpp#L322-L323>) | 错误等于“名称无效” | 推断无效名称，选择对应提示。 |
| A5 | [index_client.cpp:331–332](<E:/Projects/pulse/src/index/index_client.cpp#L331-L332>) | 状态不等于“找不到 Pulse.Index.exe” | 决定是否把连接失败状态覆盖为“索引未连接”；显示文字同时承担辅助程序缺失标识。 |

A1 的生产端确实把取消表示为“失败 + 中文字符串”：[ops_manager.cpp:3005–3008](<E:/Projects/pulse/src/ops/ops_manager.cpp#L3005-L3008>)。其他路径包括 [重命名请求校验／取消](<E:/Projects/pulse/src/ops/ops_manager.cpp#L3116-L3119>)、[批量重命名取消](<E:/Projects/pulse/src/ops/ops_manager.cpp#L3166-L3168>)。

并非缺少机器状态：[OpPhase 定义](<E:/Projects/pulse/src/ops/ops_manager.h#L38-L41>) 已有 `Cancelled`。建议统一取消路径使用该状态，而非依赖文案补判。

A3／A4 的生产端在 [名称校验与目标存在检查](<E:/Projects/pulse/src/ops/ops_manager.cpp#L259-L275>) 同时给出数值错误码；单个重命名也在 [3216–3219 行](<E:/Projects/pulse/src/ops/ops_manager.cpp#L3216-L3219>) 保存 `failure_hr`。界面却仍比较文字。新建的显示错误由 [DescribeCreateError()](<E:/Projects/pulse/src/shell_host/main.cpp#L364-L393>) 从 HRESULT／Win32 错误转换后传出。

### 3.2 翻译结果被当作横幅类型

| 编号 | 位置 | 比较的翻译资源 | 后果 |
|---|---|---|---|
| B1 | [address_search.cpp:363–378](<E:/Projects/pulse/src/app/address_search.cpp#L363-L378>) | `ContentIndexPartial`、`ContentCoverageHint`、`ContentIndexBuilding`、`ContentIndexPaused` | 从标题反推横幅是否属于内容索引，再清除或更新。 |
| B2 | [app_navigation.cpp:568–569](<E:/Projects/pulse/src/app/app_navigation.cpp#L568-L569>) | `SearchIncomplete` | 搜索成功结束后判断是否清除旧错误横幅。 |
| B3 | [app_navigation.cpp:571–572](<E:/Projects/pulse/src/app/app_navigation.cpp#L571-L572>) | `ContentIndexScanRunning` | 判断是否清除扫描中横幅。 |
| B4 | [app_main.cpp:1621–1623](<E:/Projects/pulse/src/app/app_main.cpp#L1621-L1623>) | `Offline` | 网络恢复后判断是否清除离线横幅。 |

这些字符串虽来自资源文件，依然不是机器状态。存储标题与当前语言不一致、翻译措辞发生变化或不同横幅文字相同，都存在误判风险；本轮仅确认静态耦合，未运行复现。

B4 只影响横幅清除：`net_readonly = false` 在比较之前执行，不应描述为“依靠离线文字恢复写权限”。建议保存独立 `BannerKind`／消息类别，按类别更新与清除横幅，展示时才读取资源。

## 4. 尚未修复：显示文字决定界面颜色／图标

| 编号 | 位置 | 判断 | 用途 |
|---|---|---|---|
| C1 | [ui_renderer_internal.h:1362–1370](<E:/Projects/pulse/src/ui/ui_renderer_internal.h#L1362-L1370>) | 状态包含“失败”“非 NTFS”“不支持”“正在”“等待”“就绪”“实时” | 推断索引磁盘徽章类别。实际调用在 [ui_settings_view.cpp:216](<E:/Projects/pulse/src/ui/ui_settings_view.cpp#L216>)；翻成英文后这些中文关键词不会命中。 |
| C2 | [ui_pane_list.cpp:1672–1673](<E:/Projects/pulse/src/ui/ui_pane_list.cpp#L1672-L1673>) | `type_text == Get(ChangeDeleted)` | 判断删除变化记录，在 [1715–1719 行](<E:/Projects/pulse/src/ui/ui_pane_list.cpp#L1715-L1719>) 使用特殊颜色。 |
| C3 | [ui_pane_list.cpp:1765–1769](<E:/Projects/pulse/src/ui/ui_pane_list.cpp#L1765-L1769>) | 元数据等于 `Get(FolderSizeCalculate)` | 平铺／内容视图中从“点击统计”文字反推手动统计状态，用强调色。 |
| C4 | [ui_pane_list.cpp:1781–1784](<E:/Projects/pulse/src/ui/ui_pane_list.cpp#L1781-L1784>) | 大小标签等于 `Get(FolderSizeCalculate)` | 图标视图中同样从显示文字反推状态。 |
| C5 | [ui_renderer.cpp:619–620](<E:/Projects/pulse/src/ui/ui_renderer.cpp#L619-L620>)、[638–639](<E:/Projects/pulse/src/ui/ui_renderer.cpp#L638-L639>) | 标签标题等于 `Get(Settings)` | 推断设置页并使用齿轮图标；普通文件夹或标签别名恰好与设置标题相同也可能误识别。 |

C1 的文案由 [index_config.cpp:409–410](<E:/Projects/pulse/src/index/index_config.cpp#L409-L410>) 和 [index_engine.cpp:469–477](<E:/Projects/pulse/src/index/index_engine.cpp#L469-L477>) 等位置生成；[VolumeInfo](<E:/Projects/pulse/src/index/index_config.h#L17-L30>) 当前将状态展示文字保存在 `std::wstring state`。

C2 的事件类别在 [app_change_tracking.cpp:228–241](<E:/Projects/pulse/src/app/app_change_tracking.cpp#L228-L241>) 转成 `change_type_text`，随后渲染层又从文字反推类别。建议保留明确事件类型。

C3／C4 上游已有枚举：[folder_sizes_ui.cpp:57–70](<E:/Projects/pulse/src/app/folder_sizes_ui.cpp#L57-L70>) 按 `FolderSizeState` 生成大小标签与可操作项。建议把状态或样式类别一并传给渲染层，而非逆向解析显示文字。

## 5. 尚未修复：英文错误显示文本也参与机器判断

本节是技术问题清单，不涉及任何作者或版本归属。

### 5.1 删除畸形结果文字参与可信度判断

- [delete_operation.cpp:432–436](<E:/Projects/pulse/src/ops/delete_operation.cpp#L432-L436>) 将 `error == L"Malformed deletion result"` 纳入 `uncertain`。
- [delete_operation.cpp:456–464](<E:/Projects/pulse/src/ops/delete_operation.cpp#L456-L464>) 将相同条件纳入 `backend_lost`，影响是否探测锁占用以及回收撤销记录失效处理。
- 来源是 [shell_client.cpp:418–421](<E:/Projects/pulse/src/ipc/shell_client.cpp#L418-L421>)：发现删除响应数组或尾部数据不合法后设置 `E_INVALIDARG` 和该英文错误文字。

这不只是提示样式问题。显示错误文字参与了删除结果可信度与撤销处理，后续优先级应高于纯颜色／图标判断。建议增加独立的协议响应有效性／后端结果类别，或使用专用错误码，不通过可翻译文字判断。

### 5.2 从显示错误文字提取失败路径，再探测占用进程

[FailedName()](<E:/Projects/pulse/src/ops/file_lock_owner.cpp#L130-L137>) 使用 `rfind(L" | ")` 获取尾部，再按 `L"; delete diagnostics:"` 截断诊断文字，将剩余内容当作失败路径／文件名。

[ProbeFileLocks()](<E:/Projects/pulse/src/ops/file_lock_owner.cpp#L149-L160>) 用这个值选择探测对象。删除路径在 [delete_operation.cpp:459–462](<E:/Projects/pulse/src/ops/delete_operation.cpp#L459-L462>) 确实把整条显示错误 `error` 传入，而非独立失败路径。

建议在 IPC／操作结果中单独保存精确失败路径、错误码和诊断信息。显示消息可以包含这些信息，但不应作为恢复机器字段的输入。分隔符、诊断措辞、文件名字符或翻译变化均可能影响解析。

### 5.3 从 7z 错误输出猜测是否需要密码

[archive.cpp:160–161](<E:/Projects/pulse/src/ops/archive.cpp#L160-L161>) 的 `PasswordError()` 判断输出是否包含 `password`、`Password` 或 `encrypted`。在 [ListArchive():185–188](<E:/Projects/pulse/src/ops/archive.cpp#L185-L188>) 的非零退出分支，完整输出同时用于显示错误和推断 `password_required`。

最终 [archive_navigation.cpp:175–178](<E:/Projects/pulse/src/app/archive_navigation.cpp#L175-L178>) 据此弹密码框，确认后重新读取压缩包。

这是外部工具输出启发式，受工具能力限制，性质不同于内部状态枚举。但完整输出中的路径／文件名也会参与匹配，非密码错误恰好包含关键词时存在误触密码交互的风险。本轮没有动态复现。建议优先采用工具可靠的结构化结果；若工具无法明确区分，应限定启发式匹配范围并提供通用失败退路，不把整段输出当可靠错误类型。

## 6. 排除项与审查边界

以下命中没有作为上述生产状态耦合问题列入：

- [ctx_menu_util.h:241–358](<E:/Projects/pulse/src/ipc/ctx_menu_util.h#L241-L358>)：对 Explorer／第三方右键菜单显示文字的分类，属于外部兼容启发式。它有语言与措辞依赖，但不是内部错误文案充当状态码。
- [search_kinds.cpp:38–48](<E:/Projects/pulse/src/index/search_kinds.cpp#L38-L48>)、[search_query.cpp:203–210](<E:/Projects/pulse/src/app/search_query.cpp#L203-L210>)、[index_query.cpp](<E:/Projects/pulse/src/index/index_query.cpp>)：中文搜索别名与语法是用户输入协议，不应搬成当前界面语言才能识别的翻译资源。
- [app_model.cpp:1405–1410](<E:/Projects/pulse/src/app/app_model.cpp#L1405-L1410>)：标签简称输入匹配，不是错误／状态类型判断。
- [archive.cpp:202–221](<E:/Projects/pulse/src/ops/archive.cpp#L202-L221>)：7z `-slt` 的 `Path =`、`Folder = +`、`Encrypted = +` 是结构化文本协议，与从错误输出猜密码类别不同。
- 测试中对中文消息的比较是断言，不等同于生产流程依赖文案。是否应降低测试对具体措辞的耦合需按用例目的判断。
- 预览模块的 `path-unavailable` 等机器 reason token 再映射为资源显示，不作为显示文案状态码问题。
- 为重绘、去重或检查显示文本是否变化而比较字符串，不直接等同于把字符串当状态类型。

审查使用文字匹配、资源比较、Unicode 转义与间接辅助函数检查，并读取相关上下文和生产端／消费端。结论是当前源码的静态确认，不等于已触发所有风险或证明所有后台硬编码都属于此类问题。

## 7. 后续修复建议与当前状态

建议顺序：

1. **删除安全／结果可信度：** 畸形响应分类和失败路径独立字段，避免安全处理依赖展示文本。
2. **操作结果：** 取消统一 `OpPhase::Cancelled`，错误分类使用错误枚举／HRESULT／Win32 错误码；补齐批量操作的结构化错误类型，避免把普通创建失败归成权限问题。
3. **横幅模型：** 独立保存横幅类别与参数，更新／清除按类别操作。
4. **渲染元数据：** 索引状态、变化事件、文件夹大小状态、设置页类型传递明确枚举／标志。
5. **后台显示文案资源化：** 在机器字段与显示文本分离后，将剩余用户可见后台文案迁移到资源，避免因直接替换翻译破坏既有判断。
6. **外部工具：** 单独处理 7z 输出启发式，覆盖密码错误、普通失败和输出含关键词的非密码情况。

以上建议本轮没有实施，不应把报告生成或资源表合并视为这些问题已修复。任务不涉及文件操作、网络探测、索引服务或搜索行为重构。

## 8. 实际验证与限制

### 8.1 界面文案资源化阶段已经执行

- 构建 `pulse`、`pulse_localization_test`、`pulse_link_confirmation_ui_test` 成功。
- `pulse_localization_test --centralized`：两种语言的新增资源存在、代表文案及前后空格检查通过。
- 中文深色链接确认窗口：截图生成与键盘确认行为通过。
- 英文浅色退出确认窗口：截图生成与 Esc 保持任务运行的行为通过。
- 检查 [链接确认截图](<E:/Projects/pulse/bench_data/link_confirmation_dark.png>) 和 [英文退出确认截图](<E:/Projects/pulse/bench_data/exit_confirmation_light_en.png>)，当时未见裁切或重叠。之后用户改动过资源文案，这些截图不是当前全部翻译的重新视觉验收。
- 静态检查当时 1936 条资源的两种语言集合、重复项、ID 定义及列对齐通过；本轮相关 `git diff --check` 通过。

### 8.2 本轮字符串表合并

- 合并操作逐条比较当前全部 1936 条“语言 + ID + 原始文本字面量”，一致性检查通过。
- 76 个单条表合并为 4 个表，全部条目保留，当前用户调整的文案保持不变。
- 仅定向构建 `pulse` 和 `pulse_localization_test`，资源编译与链接成功；本轮日志显示主要重新编译了资源，没有为报告机械扩大全量构建。
- 用 `LoadLibraryExW(..., LOAD_LIBRARY_AS_DATAFILE)` 以数据方式分别读取主程序和测试程序的 `RT_STRING`，按原始语言 ID 与数值资源 ID 核对：两个程序中的全部 1936 条编译资源均与当前源码值完全一致。程序没有作为 GUI 启动，未操作用户文件或设置。
- `pulse_localization_test --centralized` 再次通过：两种语言的新增资源和有意义空白均正确。
- 静态确认两种语言各有 968 个 ID、没有重复条目、资源文本列仍从第 43 列开始；报告不含作者归属，全部文件链接存在。
- 本轮相关 `git diff --check` 通过。资源合并未改变任何显示文字，未重复运行确认窗口视觉测试。

### 8.3 未执行／不能据此声称通过

- 文案状态耦合问题仅做静态审查，未实施修复、未动态复现所有风险。
- 没有运行完整文件操作／索引／搜索历史回归套件。
- 没有重新进行所有已迁移界面、所有语言、全部 DPI／窄布局的视觉验证。
- 构建成功只证明受影响目标可以编译链接，不等于上述未修复问题已经通过功能验证。

## 9. 执行过程中的环境事项

资源批量迁移期间曾出现沙箱内脚本无法写入工作区源码的异常，经授权进行了 Windows 文件权限诊断。诊断为 `src` 和 `src/ui` 添加当前登录用户的完全控制项并验证；未修改文件所有者。沙箱内脚本随后仍被拒绝，之后在允许的完整访问环境完成资源迁移。

此项不是本地化业务代码修改，但应保留环境变更记录。报告与备份位于 [权限诊断报告](<E:/Projects/pulse/permission-recovery/acl-report-4e998be45cda48b8bb6a3177b5338c90.jsonl>)。如需撤销该次权限调整，诊断给出的恢复顺序是先恢复 `src`，再恢复 `src/ui`；本报告生成时未执行恢复：

```powershell
pwsh -NoProfile -File 'E:\Projects\pulse\permission-recovery\acl-backup-f725e854b5f24ed18e8b7bca6f6e3206.json.ps1' -Path 'E:\Projects\pulse\src' -AllowRoot 'E:\Projects\pulse' -Restore 'E:\Projects\pulse\permission-recovery\acl-backup-f725e854b5f24ed18e8b7bca6f6e3206.json'
pwsh -NoProfile -File 'E:\Projects\pulse\permission-recovery\acl-backup-c1c6323aa74547cba8c53bcc194607f3.json.ps1' -Path 'E:\Projects\pulse\src\ui' -AllowRoot 'E:\Projects\pulse' -Restore 'E:\Projects\pulse\permission-recovery\acl-backup-c1c6323aa74547cba8c53bcc194607f3.json'
```
