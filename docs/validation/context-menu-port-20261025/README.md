# 右键菜单移植与本地适配验证

日期：2026-10-25。参考上游 `jimmgreen/pulse` 的固定快照 `c4fe01ed82fe091f495f531972a854aa32caac2b`，来源位于 `bench_data/upstream-edit-review-20261024/`。以下结果仅针对本次右键菜单改动。

## 最终界面与行为

- 设置侧栏仅保留一个“右键菜单”入口，页内切换“Pulse 右键菜单”和“系统右键菜单”；共用标题、侧栏选中状态及同一个设置标签页，移除原先重复的设置界面和底部跳转卡片。
- Pulse 菜单支持精简、标准、完整预设；可见性组合不匹配预设时显示自定义状态。文件菜单、空白处菜单、文件行按钮分别提供预览和拖动排序，可恢复当前排序或全部默认值。
- 保留本地命令、原有 JSON 键与枚举编号、默认顺序、分组入口、“打开方式”位置及本地“新建”子菜单。共用命令的可见性同步，三个界面的顺序独立保存。
- 系统菜单提供后台扫描、场景与筛选、来源和权限详情、Windows 开关与仅在 Pulse 隐藏。更改先排队，明确点击应用后写入。
- 注册表写入前必须成功备份；保存 Pulse 偏好失败时保留队列且不写入注册表。失败项保留待处理；本次会话内支持注册表撤销/还原。快照与撤销保留原注册表类型、字节与长度，包括非规范字符串。
- 独立的 `pulse_integration.exe` 承担管理员范围写入，沿用本地 Shell host/IPC 隔离。安装和便携包脚本已纳入该辅助程序；本次未制作安装包。

## 实际验证

受影响目标构建成功，日志为 [build-final.log](build-final.log)；最终卡片排版调整后再次构建主程序及界面测试目标成功，见 [build-final-layout.log](build-final-layout.log)，无编译警告。不含完整历史回归。

| 定向程序 | 通过 | 失败 | 主要覆盖 |
| --- | ---: | ---: | --- |
| [pulse_builtin_menu_order_test](pulse_builtin_menu_order_test.log) | 21 | 0 | 顺序规范化、持久化、实际菜单及本地命令边界 |
| [pulse_builtin_menu_items_test](pulse_builtin_menu_items_test.log) | 20 | 0 | 隐藏项、固定动作、分隔线、文件行按钮、中英文标签 |
| [pulse_system_ctx_test](pulse_system_ctx_test.log) | 63 | 0 | 隔离注册表枚举、应用/撤销、原始值恢复、备份与辅助程序输入校验 |
| [pulse_system_ctx_model_test](pulse_system_ctx_model_test.log) | 31 | 0 | 排队/应用、备份失败、偏好保存失败、部分失败及批次大小边界 |
| [pulse_context_menu_settings_ui_test](pulse_context_menu_settings_ui_test.log) | 4858 | 0 | 生产控制器/渲染器、统一入口、双向切换、拖动与命中、菜单及行按钮、卡片完整文字和自适应布局 |

测试程序从构建目录复制为字节相同的普通临时目录副本后运行，SHA256 记录见 [test-copy-hashes.json](test-copy-hashes.json)。注册表测试记录的进程为 Medium Integrity（8192）、非管理员。夹具使用独立的新建注册表根及文件目录，仅清理自己成功创建的夹具；界面测试使用隔离偏好目录。

实际环境只读扫描见 [system-menu-readonly-scan.log](system-menu-readonly-scan.log) 与 [system-menu-readonly-scan.tsv](system-menu-readonly-scan.tsv)：读取 199 项场景记录，其中 30 项禁用、1 项目标文件缺失，未修改这些真实注册项。场景间可能重复列出同一注册项，这些数字不是唯一命令数。

## 界面证据与边界

[screenshots](screenshots/) 包含 104 张生产 Direct2D 渲染器输出：中英文、760/1100 逻辑宽度、100%/150% 缩放、深浅色；覆盖三个 Pulse 界面与系统层加载、空、错误、就绪状态。所有矩阵均保存系统就绪和错误截图，宽窗口100%额外保存加载与空状态。顶部卡片在窄窗口上下排列，标题与说明完整显示。最终人工目检了中文宽版 Pulse、中文窄版系统详情与文件行按钮、英文150%窄版两层及英文宽版错误状态。系统层截图使用注入显示数据，不代表真实注册表写入测试。没有可用的改动前截图，不宣称完成前后图对照。

可直接查看：[中文宽版 Pulse](screenshots/zh-CN-1100-100-dark-pulse-controls.png)、[中文窄版系统菜单](screenshots/zh-CN-760-100-light-system-ready.png)、[英文150%窄版](screenshots/en-US-760-150-dark-pulse-controls.png)。

- 注册表功能验证使用隔离 HKCU 根模拟当前用户与管理员两个范围；未在用户真实 HKLM 上执行写入，也未触发真实 UAC 提权流程。
- 多项应用按项执行，不是跨注册表键的原子事务；界面保留失败项并允许撤销已成功的注册表改动。
- 撤销与全部还原覆盖本次会话记录的注册表改动；仅在 Pulse 隐藏的偏好不纳入注册表撤销。
- 本次没有替换已安装程序、修改用户个人文件或真实菜单注册配置，也没有提交/推送工作区修改。
