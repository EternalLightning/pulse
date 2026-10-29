# App integrations 源码审查

日期：2026-10-30。范围：`src/app` 中按分区规则匹配的应用集成文件，并包含 `explorer_window_takeover.*`；排除已交测试分区的 `*_test.cpp`、`selftest_1b2.*`。88/88 个文件已完整阅读，机器清单见 `app-integrations-coverage.json`。

仅写本报告及覆盖清单；没有修改源码、构建、运行测试、操作真实设备、写注册表或更改用户配置。“确认”指静态代码和调用链确认；运行表现、时间与环境条件仍需定向验证。未确认 P0/P1 问题。

## AI01 — P2：超时的旧设备列表仍可覆盖新连接状态

- **位置**：[device_registry.cpp:177](E:/Projects/pulse/src/app/device_registry.cpp:177)、[device_registry.cpp:189](E:/Projects/pulse/src/app/device_registry.cpp:189)；支持链：[device_worker.cpp:99](E:/Projects/pulse/src/fs/device_worker.cpp:99)、[device_shell.cpp:105](E:/Projects/pulse/src/fs/device_shell.cpp:105)。
- **触发条件**：设备列表请求 A 卡在驱动/Shell 调用中并超过 15 秒预算；设备到达、移除或解锁又触发请求 B，B 在替代线程先完成；A 随后返回。
- **事实**：`OnTimer`（70–73）提交刷新但不携带请求代数；`RefreshOnWorker`（172–177）只在进入前检查停止状态，随后直接 `Publish(ListDevices())`。`Publish`（180–195）只拒绝已停止实例，不检查请求新旧，任何不同列表都能替换 `devices_`、递增 `revision_`、清空连接目录 ID。worker 超时只是放弃旧 lane（99–109），正在执行的 `task.run()`（62）会继续。Shell 的 `ListDevices`（105–144）没有取消参数。
- **推断与影响**：A 可以将 B 已确认的移除、解锁或重连状态回退，使侧栏重新显示过期设备/存储或锁定状态，并让 `FolderShellId`（317–329）再次返回旧列表中的存储标识。`revision_` 是发布次数，不能区分过期请求。
- **建议**：在刷新请求产生时分配单调代数；只允许当前请求在持有 registry 锁时发布。连接变化时同时更新目录缓存所属 epoch，写入与读取都核对它。
- **待实测**：用可控 source 将 A 的列表返回阻塞，令 B 先发布，再释放 A，确认最终列表保持 B。目录枚举另有较小竞态：262–263 在检查 `job->done` 前写缓存；但 Shell 枚举在 `device_shell.cpp:192` 有取消终检，阻塞中取消通常会返回 Cancelled。不能将“任何取消都会回写”作为已确认事实；需在终检之后注入同步点再验证。
- **分区关系**：与 fs 分区的“计时预算绑定旧 lane、排队后失效”是不同根因；此处要求的是发布结果的新旧校验。

## AI02 — P2：清空标签时删除 ADS 失败被当作成功，旧标签可被重新导入

- **位置**：[places.cpp:1535](E:/Projects/pulse/src/app/places.cpp:1535)；支持链：[app_commands.cpp:196](E:/Projects/pulse/src/app/app_commands.cpp:196)、[places.cpp:1495](E:/Projects/pulse/src/app/places.cpp:1495)、[app_main.cpp:1872](E:/Projects/pulse/src/app/app_main.cpp:1872)。
- **触发条件**：移除文件的最后一个标签或删除标签，文件上已有 `:Pulse.Tag`，但该 ADS 因权限、共享或设备错误无法删除。
- **事实**：`WriteTagAdsV2`（1535–1537）对空标签执行 `DeleteFileW` 后无条件返回 true。`QueueTagAds`（189–210）只在该函数返回 false 时记录并显示失败位置，因此这里无法触发提示。目录探测仍读取 ADS（`app_runtime.cpp:2400–2402`），消息处理随后调用 `MergeAdsRecords`（`app_main.cpp:1867–1873`），后者把 ADS 中仍存在的记录重新加入标签路径（1472–1504），必要时还会重新建立已删除的标签。
- **推断与影响**：用户看到已移除标签且没有失败提示；重新发现或重启后，旧标签可能再次出现。此处不能承诺所有 ADS/文件系统错误都可自动修复，但必须准确报告失败。
- **建议**：只把删除成功、明确不存在的 ADS 当作成功；保留并返回其他错误，使现有 warning 路径生效。若产品承诺移除后不会被旧 ADS 回填，还需定义失败期间的同步/重试或本地移除记录策略。
- **待实测**：在隔离文件上先写入标签，再令 ADS 删除返回拒绝访问；移除最后标签应产生失败提示。恢复访问并重新探测，核对最终选择的同步语义。

## AI03 — P2：缺失图标会将仍可运行的系统菜单命令列为可删除残留

- **位置**：[system_ctx_registry.cpp:321](E:/Projects/pulse/src/app/system_ctx_registry.cpp:321)；删除判定：[system_ctx_model.cpp:45](E:/Projects/pulse/src/app/system_ctx_model.cpp:45)、[system_ctx_model.cpp:1464](E:/Projects/pulse/src/app/system_ctx_model.cpp:1464)。
- **触发条件**：一个非保护静态 verb 的 `command` 指向存在且可运行的程序，但单独的绝对 `Icon` 路径已经失效。例如命令是现存的 `C:\\App\\tool.exe "%1"`，图标仍指向已删除的 `C:\\Old\\tool.ico`。
- **事实**：扫描先成功解析命令程序（319），随后把“命令缺失 **或** 图标缺失”统一赋给 `item.missing`（321）。`Deletable`（model:45）仅要求 `missing && Toggleable`；“清理残留”遍历所有满足条件的项并排队删除（1458–1470），最终 `ApplyTrash` 移动整个 verb 注册表树（registry:810–823），没有重新判定命令是否存在。
- **推断与影响**：只想清理卸载残留的用户会把功能仍正常的命令一起移出 Explorer 菜单。当前有备份和会话撤销，因此不能称为不可恢复删除，但清理资格与“程序已不存在”的提示不符。
- **建议**：把执行目标缺失、图标缺失和读取失败分成不同状态。仅明确失效的执行目标允许自动残留清理；失效图标只提示或降级显示。无法访问也不应直接解释为不存在。
- **待实测**：在隔离 `HKCU\\Software\\PulseTest\\...` 根建立“命令存在、图标缺失”的 verb，枚举时应保留可用状态且不允许批量清理；另覆盖真正缺失的命令。

## AI04 — P2：注册表树移动无法确认恢复成功时仍返回普通失败

- **位置**：[system_ctx_registry.cpp:769](E:/Projects/pulse/src/app/system_ctx_registry.cpp:769)、[system_ctx_registry.cpp:775](E:/Projects/pulse/src/app/system_ctx_registry.cpp:775)。
- **触发条件**：`MoveTree` 已复制目标树，随后源树删除部分失败；尝试复制回源树又失败，或清理回滚副本失败。
- **事实**：764–775 明确考虑了删除中途失败，并计算 `restored`。当 `restored == false` 时会保留目标副本，但仍统一 `return FromStatus(st)`，不会返回现有的 `RecoveryRequired`。754–759 的初次复制失败分支同样忽略目标清理结果。上层只将 Ok 加入 `done`、RecoveryRequired 加入 `recovery`（model:893–901）；只有这两类会形成可撤销/恢复记录（937–947）。提权助手也只对 Ok/RecoveryRequired 标记 changed（registry:1607–1613）。
- **推断与影响**：源树可能已部分改变或存在需要处理的副本，但页面按普通失败处理，不进入已有的“结果无法确认，需要恢复”路径；再次执行还可能因副本已存在而冲突。在仅有此失败的提权批次中，Shell 变化通知也可能缺失。完整副本和 .reg 备份可能仍在，不能据此断言数据丢失。
- **建议**：将回滚失败、清理失败映射为 `RecoveryRequired`，保留源/目标路径和原始错误。恢复策略需能识别部分源树与完整副本，避免普通 inverse 仅因源目标都存在就再次 Conflict。
- **待实测**：隔离注册表故障注入：源树删除失败并令回拷失败；确认返回恢复状态、保留可定位恢复材料、显示正确提示并通知 Shell。现有值级回滚已经区分 RecoveryRequired（638–656），可沿用其语义。

## AI05 — P2：批量云操作按文件重复刷新父目录，UI 工作随选择量平方增长

- **位置**：[cloud_commands.cpp:13](E:/Projects/pulse/src/app/cloud_commands.cpp:13)、[cloud_commands.cpp:139](E:/Projects/pulse/src/app/cloud_commands.cpp:139)；支持链：[app_navigation.cpp:1405](E:/Projects/pulse/src/app/app_navigation.cpp:1405)、[app_navigation.cpp:1477](E:/Projects/pulse/src/app/app_navigation.cpp:1477)。
- **触发条件**：在同一可见云目录中多选 N 个文件，执行“始终保留”或“释放空间”，完成消息返回该 N 个路径。
- **事实**：`RunPin` 保存完整选择列表（34–46）；完成处理（136–139）将它交给 `RefreshCloudPaths`。后者对每个路径分别计算父目录并调用 `RefreshPath`（13–18），未去重。`RefreshPath` 每次都会遍历可见 pane、捕获选择、清零 pending generation、提交一次 worker refresh（1426–1500）。选择捕获每次再遍历所有 N 个选中项并复制名称（1400–1413）；`PrepareEntryRefresh` 不会清除这些选中项（`app_model.cpp:136–142`）。
- **推断与影响**：在完成消息的 UI 线程处理（`app_main.cpp:1832–1833`）中，单目录 N 文件至少引入 N 次目录刷新准备和约 N×N 次选择项处理；大量选择会造成可避免的卡顿与请求取消抖动。
- **边界**：不能称为 N 次完整磁盘枚举。worker 同 sort key 会新建 producer（`app_worker.cpp:222–236`），但 `PruneDirectories` 会丢弃过期请求（97–110、246）；真正开始和完成的 IO 次数取决于调度和取消时点。
- **建议**：先按规范化父目录去重，每个目录只 MarkDirty/RefreshPath 一次；同一消息批次的完成项也可以合并。现有设备导航刷新已采用路径去重，可保持一致。
- **待实测**：用隔离云状态源或函数边界计数器，给 1、100、1000 个同父目录路径，确认每个唯一父目录只触发一次刷新；测量 UI 消息处理耗时，并保留跨目录情况。

## 尚需实测的风险（不计为已确认缺陷）

- **混合公共/私有网卡发现边界**：`lan_discovery.cpp:374–403` 在任一私有/域网络存在时允许发现；mDNS 请求接口索引为 0（185），WSD 查询未见按网络类别筛选接口。头部宣称 Public 网络不发发现，但结构注释（`lan_discovery.h:21`）又明确只有所有连接均 Public 才停止。需要在私有有线 + 公共 Wi-Fi 双连接环境抓包，确认 Windows API 实际选择的接口，并先明确产品契约。没有抓包证据，不断言已向公共网卡发送数据。

## 设计与重复实现判断

- Explorer 接管的身份、标签数、当前路径、选中项与输入时间复核分别保护提交和关闭窗口；默认文件管理器的 Pending/旧值备份/外部变更检测保护可恢复性。它们有不同失败边界，不应仅因检查相似就合并删除。
- Pulse 自有菜单偏好与系统注册表菜单管理具有不同作用域。当前文本比较由统一 `text_compare_enabled` 控制，本次没有提出重新增加独立菜单开关。
- 云探测、网络连接和设备 Shell 工作均有后台路径；没有建议为减少代码而移回 UI 线程。平台能力探测与必要 fallback 不计为重复设计缺陷。
- 本次没有足够证据将其余文件判为无用代码或可直接删除；不以“未看到调用”代替跨目标/测试入口证明。

## 完整读取清单

以下 88 项均实际完整阅读；跨分区调用链只做定向读取，不把其整个文件计入覆盖。

- `src/app/app_network_access.cpp`
- `src/app/app_network_access.h`
- `src/app/app_network_locations.cpp`
- `src/app/app_network_locations.h`
- `src/app/breadcrumb_menu.cpp`
- `src/app/builtin_menu_items.cpp`
- `src/app/builtin_menu_items.h`
- `src/app/builtin_menu_strings.h`
- `src/app/cloud_commands.cpp`
- `src/app/cloud_commands.h`
- `src/app/cloud_download.cpp`
- `src/app/cloud_download.h`
- `src/app/cloud_probe_access.h`
- `src/app/cloud_root_catalog.cpp`
- `src/app/cloud_root_catalog.h`
- `src/app/cloud_roots.cpp`
- `src/app/cloud_roots.h`
- `src/app/cloud_sync_probe.cpp`
- `src/app/cloud_sync_probe.h`
- `src/app/context_menu.cpp`
- `src/app/context_menu.h`
- `src/app/context_menu_controller.cpp`
- `src/app/context_menu_controller.h`
- `src/app/context_menu_prefs.cpp`
- `src/app/context_menu_prefs.h`
- `src/app/context_menu_settings_drag.cpp`
- `src/app/context_menu_settings_drag.h`
- `src/app/default_file_manager.cpp`
- `src/app/default_file_manager.h`
- `src/app/device_messages.h`
- `src/app/device_navigation.cpp`
- `src/app/device_navigation.h`
- `src/app/device_registry.cpp`
- `src/app/device_registry.h`
- `src/app/explorer_takeover_policy.h`
- `src/app/explorer_window_takeover.cpp`
- `src/app/explorer_window_takeover.h`
- `src/app/folder_view_prefs.cpp`
- `src/app/folder_view_prefs.h`
- `src/app/lan_device.cpp`
- `src/app/lan_device.h`
- `src/app/lan_discovery.cpp`
- `src/app/lan_discovery.h`
- `src/app/network_connect.cpp`
- `src/app/network_connect.h`
- `src/app/network_locations.cpp`
- `src/app/network_locations.h`
- `src/app/network_recent.cpp`
- `src/app/network_recent.h`
- `src/app/network_recent_store.cpp`
- `src/app/network_recent_store.h`
- `src/app/network_sidebar.h`
- `src/app/places.cpp`
- `src/app/places.h`
- `src/app/quick_access.cpp`
- `src/app/quick_access.h`
- `src/app/quick_access_menu.h`
- `src/app/server_address.cpp`
- `src/app/server_address.h`
- `src/app/settings_controller.cpp`
- `src/app/settings_controller.h`
- `src/app/settings_transfer.cpp`
- `src/app/settings_transfer.h`
- `src/app/settings_transfer_ui.cpp`
- `src/app/settings_transfer_ui.h`
- `src/app/settings_ui.cpp`
- `src/app/settings_value_dialog.cpp`
- `src/app/settings_value_dialog.h`
- `src/app/shell_registry_watch_context.h`
- `src/app/shell_window_registry.cpp`
- `src/app/shell_window_registry.h`
- `src/app/sidebar_pin_menu.cpp`
- `src/app/sidebar_pin_menu.h`
- `src/app/sidebar_resize.h`
- `src/app/sidebar_scrollbar_fade.h`
- `src/app/sidebar_sections.cpp`
- `src/app/system_ctx_apply.h`
- `src/app/system_ctx_exchange.h`
- `src/app/system_ctx_model.cpp`
- `src/app/system_ctx_model.h`
- `src/app/system_ctx_preview_drag.cpp`
- `src/app/system_ctx_preview_drag.h`
- `src/app/system_ctx_registry.cpp`
- `src/app/system_ctx_registry.h`
- `src/app/system_ctx_settings.cpp`
- `src/app/system_ctx_settings.h`
- `src/app/system_network_locations.cpp`
- `src/app/window_registry.h`

## 验证与限制

- 已完成本分区源码全读、关键发布/恢复/标签/云刷新调用链的定向复核、跨分区证据核对。
- 未构建、未运行测试或 GUI、未使用真实 MTP/云盘/网络接口或写真实注册表。以上待实测项均未标为通过。
- 所有建议均为后续修复建议，本报告没有实现修复。

