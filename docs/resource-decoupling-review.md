# 资源状态解耦：审阅、实施与验证

审阅日期：2026-10-05。以当前工作区为准；保留已有未提交代码、翻译、构建脚本和删除的发行说明。

> 后续已按“保持用户可观察行为不变”收敛部分搜索合并、菜单数据和编译依赖；该阶段范围及验证单独记录在 [行为保持优化报告](E:/Projects/pulse/docs/architecture-preserving-optimization.md)。本文的验证记录仅对应资源整改阶段。

## 判断原则

功能接口以实际生产调用为准，每个责任只有一个实现入口。请求、结果、取消和展示各自有真实需求时才保留边界，不为了分层而增加转发器。跨进程隔离、异步取消和不同数据源不是重复样式，不能仅凭接口数量删除。

状态由枚举、错误码、对象身份或精确路径表达；资源只负责展示。已有字段能表达的状态直接复用，禁止把状态先变成文字再从文字还原。性能结论区分源码可证明的复制成本与尚未测量的耗时。

## 已确认问题与最小整改

| 审计项 | 当前证据 | 修改边界 |
|---|---|---|
| A1–A4 | `OpsManager` 部分取消产生 Failed；`UpdateOperationUi` 比较中文/英文错误文案；单个重命名已有数值码而批量未完整传递 | 统一 Cancelled；透传已有 HRESULT；UI 按错误码选提示，不增加错误分类框架 |
| 5.1 | `ShellClient::ReaderThread` 把删除尾部错误标为 E_INVALIDARG 和文字；`RunDelete` 再比较该文字决定结果可信度 | 为畸形删除响应使用一个专用 HRESULT，严格校验响应；不并存第二套错误类型 |
| 5.2 | Shell 进度接收器已有失败项，但仅拼到显示消息；`ProbeFileLocks` 切割 ` | ` 和诊断后缀还原对象 | 删除响应末尾传失败路径；操作层直接消费字段；无失败对象证据不猜测任意候选 |
| B1–B4 | 搜索、离线横幅的清除逻辑比较资源标题 | 仅在模型保存横幅来源类别，所有生产端与清理端同步；标题仍用于显示 |
| C1 | 本地卷已有 online/supported/enabled/progress，但失败仍由 error 文案表达，UI 用中文关键词推断徽章 | 既有 flags 增加失败位，在模型构造时得到已有展示类别，不增加 IPC 记录布局 |
| C2–C4 | 变化事件和文件夹大小都有上游机器状态，渲染却比较显示文字 | 保留已有状态到行展示模型，颜色按状态决定 |
| C5 | 标签已有路径/页面身份，但齿轮图标按“设置”的翻译标题判断 | 使用页面身份；同名普通目录仍使用目录图标 |
| A5 | 索引客户端通过“找不到 Pulse.Index.exe”文字保护缺失状态 | 在连接失败处直接检查辅助程序存在性/机器结果 |
| 5.3 | `PasswordError` 在整段 7z 输出搜索 password/encrypted，文件路径也参与匹配 | 限定为已知诊断整行，与请求的压缩包路径一致；保留 `Encrypted = +` 的结构化成员状态 |

## 7z 实测证据

使用仓库随附 `third_party/7zip/7z.exe` 在独立临时探测目录生成加密 7z。缺密码列目录产生完整诊断行 `ERROR: <请求路径> : Cannot open encrypted archive. Wrong password?`；不存在的 `password-missing.7z` 只产生文件不存在诊断，旧实现却会把路径中的 password 认作密码错误。该问题可在既有 `pulse_archive_test` 增加独立密码筛选验证。

外部工具不提供独立密码退出码，仍需识别固定诊断；这项兼容边界应明确记录，不能声称所有版本/所有压缩格式都已得到结构化错误分类。无需引入压缩服务或新的后端实现。

## 实施与验收边界

整体功能与接口清单见 [功能接口与实现边界审阅](E:/Projects/pulse/docs/architecture-interface-review.md)。上文先记录修改前结论，再开始源码修改；下文记录实际实施和验证，避免把建议当成已完成事项。

仅构建主程序及受影响的 Shell/索引和定向测试目标。回归集中验证：消息改变不改变状态与对象识别、取消终态、错误码分类、畸形删除响应、密码关键词路径不误触、既有机器状态决定样式。测试使用独立数据目录，不修改用户偏好或文件。没有时延测量时不宣称性能提升；没有实际窗口检查时不宣称 UI 视觉验收通过。

## 已实施：操作结果和删除协议

- [ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp) 的传输、批量改名和 Shell 完成路径统一使用既有 `OpPhase::Cancelled`，清空取消错误；启动时复位失败字段，批量改名和创建保存 HRESULT/失败对象。[app_ops_ui.cpp](E:/Projects/pulse/src/app/app_ops_ui.cpp) 按权限、冲突和无效名称错误码选择资源，不再比较中文或英文错误措辞。
- [protocol.h](E:/Projects/pulse/src/ipc/protocol.h) 定义一个专用 `kMalformedDeleteResult` HRESULT 和唯一删除响应解析器。完整响应包括 HRESULT、取消、显示消息、三组删除/回收证据及独立 `failed_path`。解析器验证尾部、数组配对及路径中的嵌入终止符；畸形响应不发布部分证据。
- [shell_host/main.cpp](E:/Projects/pulse/src/shell_host/main.cpp) 从实际回调或回收结果传失败对象；[shell_client.cpp](E:/Projects/pulse/src/ipc/shell_client.cpp) 直接解码字段并结束对应请求，畸形头部也不再使删除请求一直等待。操作回调通过移动传递数组和路径。
- [delete_operation.cpp](E:/Projects/pulse/src/ops/delete_operation.cpp) 的结果可信度/撤销失效按专用错误码判断，并将失败字段传给锁探测。保持授权计划、取消、不确定恢复和不可重放语义。
- [file_lock_owner.cpp](E:/Projects/pulse/src/ops/file_lock_owner.cpp) 删除 `FailedName` 文案切割。接受精确绝对路径或明确名称；无对象字段不扫描，有名称时只对实际匹配文件注册 Restart Manager，不把未匹配候选当失败对象。

删除响应尾部有意要求主程序与 Shell host 配套部署；旧 host 不带失败对象字段的响应按畸形结果处理，不能用显示消息补猜路径。目录中已明确命名的失败子项仍可在原有有界扫描中定位；没有可靠对象时不返回猜测的占用者。

## 已实施：模型与呈现

- `app::Tab` 保存小型 `BannerType`，标题更新用 `SetBannerTitle`，清空用 `ClearBanner`。覆盖导航、搜索、内容结果/生命周期、压缩包、回收站、变化记录及网络探测全部标题生产端。内容索引、扫描中、搜索失败和离线的清理按类型；其他同名横幅不被误清。类型仅留在 app 模型，不再复制到渲染 VM。
- `VolumeInfo.failed` 在引擎失败分支设置，经既有 flags 的 bit 8 传输。模型用 online/supported/enabled/progress/failed 得到已有 `BadgeKind`；UI 只画徽章，不再解析中文 state。没有第二套卷状态枚举或新 IPC 记录布局。
- 变化记录生产端从 `ChangeKind::Deleted` 设置 `change_deleted`，行展示直接传布尔值；MovedOut 不因文案相同而使用删除色。
- 文件夹大小标签的原 map 值改为 `{text, manual}`，生产端直接用 `FolderSizeState::Manual`。平铺和图标视图从该标志选择颜色，不新建平行状态集合。原点击 action 范围保留。
- 标签展示增加 `is_settings`，从 `pulse:settings:` 页面身份产生。普通目录或别名叫“设置”不会冒充设置页；真正设置页改名仍保留设置图标。
- `IndexClient.helper_missing_` 只保护辅助程序缺失提示；启动、下一次尝试与成功连接复位。没有从状态文字恢复标志，也没有引入连接状态总线。

关键入口：[app_model.h](E:/Projects/pulse/src/app/app_model.h)、[index_volume_badge.h](E:/Projects/pulse/src/app/index_volume_badge.h)、[app_change_tracking.cpp](E:/Projects/pulse/src/app/app_change_tracking.cpp)、[folder_sizes_ui.cpp](E:/Projects/pulse/src/app/folder_sizes_ui.cpp)、[ui_pane_list.cpp](E:/Projects/pulse/src/ui/ui_pane_list.cpp)、[index_client.cpp](E:/Projects/pulse/src/index/index_client.cpp)。本次不改变资源表文字、布局模式或后台工作归属。

卷失败标志需要新版 `Pulse.Index` 生产；旧索引进程不提供该 bit，主程序不会从旧显示文字补判失败。实际部署应更新配套索引程序/服务。本次只构建配套程序，不启动或替换用户正在使用的服务。

## 已实施：7z 密码诊断

[archive.cpp](E:/Projects/pulse/src/ops/archive.cpp) 的密码判断仅匹配随附 7z 的已知加密头诊断整行，并要求其中路径与当前请求一致；成功列目录仍从 `Encrypted = +` 读取成员状态。删除了整段输出关键词搜索。ZIP 元数据列目录可以在未验证输入密码的情况下成功，不能把这个成功当作密码正确；提取阶段仍负责解密验证。

外部工具仍只有通用失败退出码，因此这里是受限的协议文本识别，未声称其他版本/所有格式都提供机器密码错误码。没有新增错误后端或重试兜底。

## 实际验证记录

| 检查 | 实际结果 |
|---|---|
| `pulse_archive_test --password-only` | 14 项通过：关键词文件名的缺失/损坏失败、加密 7z/ZIP、错误/正确密码、取消。 |
| `pulse_archive_test --process-only` | 排查环境时运行，6 项通过：句柄隔离、输出限额、尾部输出、静默子进程取消。 |
| `pulse_ops_test --filter operation-resources` | 21 项通过：生产改名/取消/创建、删除可信度/错误码/独立对象、无对象/未匹配锁探测、真实 Shell 锁定删除字段链。 |
| `pulse_ipc_payload_test` | 23 项通过，包括真实解析器的畸形删除头/尾、旧缺字段响应、回收数组不配对、路径终止符和消息变化。 |
| `pulse_file_lock_owner_test` | 1 项失败：`RmStartSession=29`，收集到 1 个文件但未进入注册对象阶段，无法报告占用 PID。修改前实现与同一测试也出现相同失败，未显示本次回归。其余夹具/共享冲突/无关错误/取消检查通过。 |
| `pulse` / `pulse_index` | 构建成功。 |
| `pulse_content_progress_ui_test --presentation-state` | 65 项通过：中英文横幅清理、同名不同状态、卷失败位传输/复位、徽章、变化类型、手动大小标签、设置身份和辅助程序缺失标志复位；生成并检查 8 张截图。 |

构建已经成功的目标：`pulse`、`pulse_index`、`pulse_archive_test`、`pulse_shell`、`pulse_ops_test`、`pulse_ipc_payload_test`、`pulse_file_lock_owner_test`、`pulse_content_progress_ui_test`。未运行完整 release 或历史 selftest。

### 展示截图检查

使用真实生产模型与渲染入口，在独立隐藏窗口验证中文深色、英文浅色，100% 缩放、1100×720。设置徽章行完整可见，设置页改名后保留齿轮图标；变化记录颜色依赖事件身份，手动大小标签颜色依赖标志。受影响区域未发现重叠或异常截断。夹具中故意设置与状态无关或相同的显示文字，用于证明资源变化不改变行为。

| 场景 | 中文深色 | 英文浅色 |
|---|---|---|
| 设置卷徽章与页面身份 | [截图](E:/Projects/pulse/bench_data/presentation-state/settings_zh.png) | [截图](E:/Projects/pulse/bench_data/presentation-state/settings_en.png) |
| 删除与移出变化记录 | [截图](E:/Projects/pulse/bench_data/presentation-state/changes_zh.png) | [截图](E:/Projects/pulse/bench_data/presentation-state/changes_en.png) |
| 平铺大小标签 | [截图](E:/Projects/pulse/bench_data/presentation-state/folders_zh.png) | [截图](E:/Projects/pulse/bench_data/presentation-state/folders_en.png) |
| 图标视图大小标签 | [截图](E:/Projects/pulse/bench_data/presentation-state/icons_zh.png) | [截图](E:/Projects/pulse/bench_data/presentation-state/icons_en.png) |

截图为生成的隔离夹具，位于忽略目录 `bench_data`。文件夹夹具未启动 Shell 图标加载，因此空图标区域不是图标功能验收；本次核对标签颜色与布局。未覆盖全部 DPI、窄窗口及语言/主题组合，也未迁移后台原始诊断资源。

### 环境与剩余限制

沙箱内默认 `D:/AppData/Temp` 的测试目录创建异常退出；改用工作区 `bench_data` 后，7z 子进程仍返回 `0xC0000142`。经自动审批在完整权限环境运行同一密码夹具，14 项全部通过。操作验证同样限定独立测试数据，未修改用户文件或偏好。

锁探测分别使用覆盖和默认 TEMP 的完整权限环境，均出现 `RmStartSession=29`。另从修改前 HEAD 编译相同测试，出现相同错误。独立 PowerShell 原生空会话调用返回 0，故不能笼统宣布 Restart Manager 不可用、或将原因归于 TEMP；C++ 测试进程中的失败根因仍未知。未删除或弱化断言，未顺带修改未受本次影响的 API 调用。

索引辅助程序通过了自定义管道清除缺失标志的隔离检查；真实缺失→找到、安装服务与真实连接生命周期没有动态验证。展示测试关闭偏好、历史和位置保存；临时启用的展示测试构建选项在验证后恢复关闭。本次未做性能 benchmark，架构清单中的复制、排序、重复扫描问题仍是审阅发现，不宣称已有性能提升。
