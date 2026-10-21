# 文本比较、标签动画与图标选中提示验证

日期：2026-10-21。保留工作区原有修改；本次没有提交、推送或替换已安装程序。

## 本次行为

- 设置 → 通用 → 文件列表新增“文本比较”，默认开启，设置可保存；关闭后移除两个比较菜单项。
- 对单个文本文件右键，始终提供“选择以比较文本”；选定后提供“与已选项目比较”。再次选择会替换比较源，切换标签不清空它。
- 将父仓库的行/字符比较与独立窗口做本地适配。读取、元数据和行比较在后台执行；取消或关闭不等待文件系统，结果通过带身份的窗口邮箱投递，丢弃过期任务。
- 活动标签下划线采用约 240ms 的先延伸、后收拢过渡。反向切换从当前跨度继续；拖动、结构变化、DPI/布局变化直接跟随新位置；遵循系统动画设置，结束后停止请求动画帧。
- 中、大、超大图标网格的每个选中框下部增加居中短蓝线；失焦窗格使用较淡颜色，单纯悬停不显示。

## 实际执行的检查

| 检查 | 结果 | 覆盖 |
| --- | --- | --- |
| `cmake --build build --target pulse` | 通过 | Release 主程序和其必要依赖；最终链接完成 |
| `pulse_tab_indicator_animation_test` | 18 PASS，0 FAIL | 两个方向、先伸长后收拢、快速反转、窄目标、系统禁用、布局变化、时间回退与长间隔 |
| `pulse_text_compare_test` | 30 PASS，0 FAIL | 菜单条件、源替换、目录/多选/虚拟项目拒绝；空文件、增删改、字符高亮、空白/大小写、取消、UTF-16、制表符、末尾换行、二进制/超限/文件失效 |
| `pulse_file_selection_ui_test` | 113 PASS，0 FAIL | 真实渲染器的三种图标网格、明暗主题、100%/150% DPI、悬停、多选、失焦；标签中间帧及结束调度；生产鼠标消息处理、菜单构造、命令分发、设置开关点击、跨标签保留比较源 |
| `pulse_text_diff_ui_test` | 8 PASS，0 FAIL | 实际比较窗口加载、分栏/合并视图、重新指定与关闭、重新打开后的结果身份、窄窗口深色主题、读取失败状态 |
| `git diff --check` | 通过 | 无空白错误；Git 提示部分已有文件后续会归一化为 CRLF |

独立窗口的四张截图已逐张检查；设置和文件菜单、网格选中提示、标签过渡截图也已检查。本次没有运行全部历史测试。

最初的网格像素检查把 `PaneBodyBounds` 当作列表起点，浅色主题出现 12 个失败。截图显示蓝线已绘制；检查改用生产 `PaneListRect` 后全部通过，未修改或降低蓝线判定条件。

最终 `build/Pulse.exe` SHA-256：`8FCBA7EC58C0E608FFE98A2493456039DE756400B9AD76A52531208795A35348`。

## 截图

- [未选比较源时的菜单](text-compare-tabs-2026-10-21/menu-before.png)、[已选比较源时的菜单](text-compare-tabs-2026-10-21/menu-after.png)
- [设置开启](text-compare-tabs-2026-10-21/settings-on.png)、[设置关闭](text-compare-tabs-2026-10-21/settings-off.png)
- [分栏比较](text-compare-tabs-2026-10-21/diff-split-light.png)、[合并比较](text-compare-tabs-2026-10-21/diff-unified-light.png)、[窄窗口深色](text-compare-tabs-2026-10-21/diff-narrow-dark.png)、[读取失败](text-compare-tabs-2026-10-21/diff-error.png)
- [浅色图标选中](text-compare-tabs-2026-10-21/icon-selection-light.png)、[150% DPI 深色图标选中](text-compare-tabs-2026-10-21/icon-selection-dark-150.png)
- 标签动画：[起点](text-compare-tabs-2026-10-21/tab-start.png)、[阶段 1](text-compare-tabs-2026-10-21/tab-phase-1.png)、[阶段 2](text-compare-tabs-2026-10-21/tab-phase-2.png)、[阶段 3](text-compare-tabs-2026-10-21/tab-phase-3.png)、[终点](text-compare-tabs-2026-10-21/tab-phase-4.png)

## 验证边界

桌面 `CopyFromScreen` 在沙箱及正常权限下均返回“句柄无效”。因此上述证据来自真实 Direct2D/DirectComposition 渲染器的交换链导出，以及生产窗口消息/菜单/命令处理；没有将其描述为真实桌面连续鼠标操作或动画播放观感已验收。独立窗口窄布局的中文工具栏仍可单行容纳，本轮未触发工具栏换行。未做真实 UNC 网络端点或人工高刷新率观感测试。

比较限制在界面提示：单文件最大 8MiB 或 100000 行；超长行仅显示前 4000 字符，行差异计算仍使用完整内容。

## 其他动画建议

本次检查发现侧栏、筛选框、标签重排和暂存卡片尚未统一遵循系统“关闭动画”设置；部分筛选/标题控件重定向时使用上一帧的值，可改为先采样当前时间再继续。后续可考虑给预览占位到实际内容、文件操作完成提示增加短淡入。本次未扩大这些行为的修改范围。
