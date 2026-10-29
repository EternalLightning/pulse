# 功能区动画、导航栏与右键菜单配置链路验证

日期：2026-10-26。保留工作区原有未提交修改；未安装或发布。

## 修复范围

- 功能区及控件下拉菜单从当前动画位置淡出并收回；右键菜单保留原有淡出。选中命令、重复点击和系统关闭动画时的行为保持可用。
- 空工作区保留标题或窄栏图标，使导航栏显隐开关有可见效果。“保存的搜索”不再出现在导航栏及分区右键菜单；已有数据和 section ID 保留。
- 新右键菜单加入后，旧 `pulse:quick-access` 目录开关与新 `pulse_items.quick_access` 并存。加载时按身份迁移，合法显式新配置优先；旧 API 读写统一路由当前开关，禁止再次记录为 Shell 目录项。
- “新建”替代两个旧子项开关后，旧隐藏位仍会过滤子项。现将创建命令归入一个 NewMenu 开关，清除退休位；仅当没有有效新配置且两个旧子项均关闭时，迁移为关闭新建组。旧部分隐藏升级为可操作的可见新建组。
- 原流程在 Shell 合并前过滤内置项，可能暴露原生替代项，后到的扩展也未完整经过同一配置规则。现保留原始命令，完成合并后统一应用显隐与顺序；新建工具栏独立于右键菜单显隐。
- 默认顺序与自定义顺序均使用设置模型；查看、排序、分组按子命令身份识别，不依赖图标或文字。

## 实际验证

相关目标 `pulse`、`pulse_menu_text_test`、`pulse_quick_access_badges_test`、`pulse_app_controllers_test`、`pulse_builtin_menu_order_test`、`pulse_context_menu_loading_test` 构建成功，无新编译警告。

| 定向检查 | 结果 |
| --- | --- |
| `pulse_quick_access_badges_test --sidebar-sections` | 21 项通过 |
| `pulse_menu_text_test --dropdown-animation` | 289 项通过 |
| `pulse_menu_text_test --toggle` | 21 项通过 |
| `pulse_app_controllers_test --context-menu-quick-access` | 15 项通过 |
| `pulse_builtin_menu_order_test --legacy-builtin` | 26 项通过 |
| `pulse_builtin_menu_order_test` | 21 项通过 |
| `pulse_app_controllers_test --builtin-menu-contract` | 515 项通过 |
| `pulse_app_controllers_test --context-menu-refresh` | 32 项通过 |
| `pulse_app_controllers_test --context-menu-overflow` | 16 项通过 |
| `pulse_context_menu_loading_test`（合成查询） | 9 项通过 |

跨菜单检查用实际菜单构建函数验证每个当前设置的隐藏、恢复、JSON 往返、所有子命令、默认/自定义顺序及预设；同时验证后到的新建模板、原生固定命令去重、同名第三方命令保留和图标变化。真实弹窗检查覆盖部分/最终空回复、目录切换、取消及加载占位关闭。

已检查独立配置下的生产截图：`sidebar-dark.png`、`sidebar-light-150.png`、`sidebar-rail-light-150.png`、`folder-menu-unified.png`，以及 `menus/` 的亮暗主题、100%/150% 菜单渲染。空工作区标题/图标和固定菜单项可见，未见本次新增重叠或裁切。

本次修改文件的 `git diff --check` 通过。未运行全量历史回归。

## 验证与兼容边界

截图运行报告 `[shell_client] pipe CreateFile err=5`；截图成功，但不能据此确认真实第三方 Shell 扩展的运行。合成查询和实际菜单窗口已验证相关合并/刷新逻辑。

部分历史目录项已被旧版本转换成 `v:文字`，丢失来源身份，无法安全区分真正第三方同名命令。本次保留这些记录；它们不再控制 Pulse 的内置命令。对明确的私有 key 执行迁移，不按显示文字删除扩展。
