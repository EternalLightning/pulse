# UI 剩余全文审查与局部修复（2026-10-16）

本轮补齐 `code-audit-outstanding-2026-10-16.md` 第 5 节列出的 18 个 UI 文件全文阅读。该旧清单的阅读缺口已补齐；不代表当前全仓所有新增文件均已审查，也不代表 UI 视觉或真实 provider 集成验收完成。工作区原有大量未提交修改全部保留，本轮未修改 `src/app`、`CMakeLists.txt` 或 `batch_rename_dialog.cpp`。

## 本轮修复

1. **颜色弹窗在小工作区、高 DPI 下超出工作区且 clamp 边界倒置**。`color_picker.cpp::Pick` 先通过 `FitPopupScale` 把 292×599 DIP 卡片及固定阴影限制到工作区，布局、GDI EDIT 字体及独立 DirectWrite 正文/图标字体使用同一个有效缩放；底部确认/取消控件随整体缩放保持在工作区内。`ConstrainPopupOrigin` 再安全限制位置，覆盖负坐标显示器。不再只固定 origin 而留下屏幕外按钮。极小工作区若连 25% 缩放也容纳不下，返回取消，不显示裁切窗口；该极端限制与实际跨屏视觉效果仍需人工确认。
2. **滚动菜单的子菜单锚点遗漏 scroll offset**。`fluent_menu.cpp::PositionSubmenu` 原使用未滚动行顶，而绘制和命中使用可见行位置。锚点计算减去 `scroll_y_`，保留已有淡入、滑动、工作区边界约束。
3. **详情面板在窄窗下与列表预留空间不一致**。`ui_details_panel.cpp::DetailsPanelRect` 原使用完整偏好宽度，`ContentRect` 则使用被窗口上限限制的宽度；例如宽窗保存 900 DIP 后缩到 1000 DIP，面板会覆盖列表区域。现在使用同一个 `DetailsPanelWidth`，扣除其包含的间距。保留用户偏好，重新加宽恢复原宽度，绘制与命中沿用同一个矩形。
4. **预览关闭后滚动总高仍包含预览井**。`DetailsContentHeightDip` 现在与 Draw/HitTest 一样在 `preview_enabled=false` 时传零高度，避免预览已关闭、元数据末尾仍可滚入空白。
5. **快速预览高对比系统色红蓝通道颠倒**。`quick_preview_window.cpp::Render` 原直接将 BGR `COLORREF` 交给接受 RGB 整数的 `D2D1::ColorF`。改为复用已有 `MakeHighContrastTheme().bg/.text`，不新增独立主题转换，不操作第三方 provider 自绘内容。
6. **高级搜索跨屏 DPI 后 native EDIT 保留旧字号**。提取 `RecreateEditFont`，创建阶段和 `WM_DPICHANGED` 共用；成功创建替代字体后先更新四个 EDIT，再释放旧字体，避免 HWND 引用已删除 HFONT。Luma 路径继续使用 compositor 的 DPI 格式。实际 100%→200% 跨屏迁移未动态验证。
7. **快速预览超过 96 个 hit metrics 时高亮消失**。`text_hit_ranges.h` 保留 96 项栈缓冲首次查询，遇到 `E_NOT_SUFFICIENT_BUFFER` 后按 DirectWrite 返回数量分配并重查，`FillHitRange` 使用完整结果。独立用例构造真实 140 行 DirectWrite layout；不靠模拟数量判断。
8. **绘制路径同步执行冷 Shell 关联 COM 查询**。`CanHost/CanHostPath` 现在只查询正/负缓存，未知扩展名先返回临时可预览，由已有 STA worker 的 `OpenCurrent` 执行 `AssocCreate/Init/GetString`，其执行包含在现有 open 超时预算内。创建或 Init 失败也写入 30 秒负缓存，使后续绘制回落 native/no-preview；未知扩展名首帧可能短暂显示 Loading。没有增加线程或在 UI paint 调用 COM。测试计数器验证冷 eligibility 不触发查询及解析后缓存一致，不激活外部 provider；没有注入真实慢 COM proxy。
9. **明确的自绘高对比配色冲突**。颜色弹窗输入框、native fallback、Luma EDIT、分隔线和 slider 轮廓复用高对比主题/系统 EDIT 色，并响应主题变化；保留表达实际颜色值的渐变与棋盘。快速预览 find EDIT、find bar、HUD 使用系统背景文字色；高对比文本选择/匹配改为系统文字色轮廓，保留文字背景对比，current match 用较粗轮廓区分；导航 hover 用轮廓，关闭 hover 使用系统 highlight 配对。文件冲突卡片在高对比下使用主题输入背景、清晰轮廓和主题文字图标，普通主题的原有配色不变。这些为 Pulse 自绘内容的局部修复，不能代表所有控件/第三方 provider 高对比验收完成。

`GetModalMessage`、操作窗口关闭流程等既有工作区修改不是本轮新增，不重复归功。除上述范围，没有机械替换原生窗口、重构线程或合并不同生命周期对象。

## 定向验证

本 UI 工作包不自行运行构建，避免与主工作包共享 build 目录并发；以下由主工作包串行构建并实测回传。受影响生产目标 `pulse`、`pulse_shell` 及下列三个测试目标最终构建退出 0、无 warning，日志为 `audit-closure-2026-10-16/build-ui-production.log`。

| 目标与 selector | 本轮覆盖 | 当前结果 |
| --- | --- | --- |
| `pulse_menu_text_test --popup-placement` | origin 四个边界，加 1920×1040 @200%、320×240、负坐标窄工作区的整体尺寸与底部按钮可达、正常缩放不变，共 11 断言；无 HWND | 11 PASS，退出 0 |
| `pulse_menu_text_test --lifecycle` | 原菜单生命周期夹具新增子菜单跟随 24px 滚动断言 | 18 PASS，退出 0 |
| `pulse_titlebar_layout_test --details-panel` | 100%、125%、150%、200% 下宽→窄→宽、列表/面板间距、阈值隐藏、preview off expansion 不影响总高、重新启用恢复总高，共 36 断言；公共 MainRenderer API，无 compositor | 36 PASS，退出 0 |
| `pulse_titlebar_layout_test --preview-text-ranges` | 真实 DirectWrite 140 行选择、短选择、空选择、空 layout，共 6 断言 | 6 PASS，退出 0 |
| `pulse_preview_handler_probe --association-cache-test` | 冷 eligibility 不调用关联 COM，显式解析正/负结果后重用缓存，查询仅一次；无 provider 激活 | 1 PASS，退出 0 |

五个 selector 共 72 PASS、0 FAIL，均退出 0；各日志位于 `audit-closure-2026-10-16/<target>-<selector>.log`（selector 文件名省略前导 `--`）。本工作包实际执行局部 `git diff --check`，退出 0；只有 Git 的 LF 转 CRLF 提示，无 whitespace error。已检查局部 diff 及 SDK HRESULT/ComPtr 接口。主工作包首次统一构建暴露 `text_hit_ranges.h` 局部变量 `small` 被 Windows RPC 的宏展开，已改为 `hit_metrics`，未 undef 系统宏；首次失败日志为 `audit-closure-2026-10-16/build-final.log`，后续构建与上述定向用例均已通过。高对比改动没有切换系统主题或真实截图，构建和用例通过不等于完整视觉验收。

## 原生窗口样式与复用结论

- advanced search、操作冲突/确认/进度窗口使用现有 Compositor/Painter、主题、字体和 BeginSurface/EndSurface；EDIT 保留输入、选择、IME 宿主功能。颜色弹窗使用已有 layered surface 与 hosted edit。没有在本次 18 文件中发现直接系统 MessageBox/TaskDialog/系统选色器作为正常主绘制流程。
- FluentMenu 的 `TOOLTIPS_CLASSW` 保留为合理原生例外：系统 tooltip 的文本、跟踪和最大宽度语义与系统辅助功能、高对比及文字缩放由平台管理；Painter::DrawTooltip 不是这些语义的完整替代。本轮关闭“只因仍为原生窗口而必须重写”的设计检查，不宣称已经完成屏幕阅读器、文字缩放、多屏和长文本视觉验收。
- `preview_handler_host.cpp` 的 overlay/第三方预览 HWND、视频目标窗口与图形失败回退有合理用途。provider 内容不由 Pulse 随意重绘。异步加载、代际取消、超时和 retired apartment 限额已阅读；本轮没有实际 Office/PDF/媒体 provider 验收。
- toolbar、settings、column strip、列表与命中的公共布局沿用现有 helper；详情面板本轮通过复用有效宽度修正偏差。Painter 已统一按钮、输入框、badge、菜单行、滚动条和文本测量；未为减少表面重复而新增并行控件体系。
- window material 的解码 worker 与 UI bitmap 上传分离，代际请求与取消已阅读；LumaText 的初始化失败清理、layout/surface cache 生命周期与原生文字 fallback 已阅读。未把磁盘/提供程序解码搬到 UI 线程。

## 留存问题与验证边界

已确认的可局部修复项均已修改；剩余为实际环境验收边界：

| 项目 | 当前代码证据 | 后续验证 |
| --- | --- | --- |
| 字体与 popup 交互 | DPI 字体和拟合尺寸已修复，几何用例通过 | 受控 100%→200% 跨屏，native/Luma 输入与小工作区底部按钮实际点击及可读性 |
| 关联 COM 集成 | cold lookup 已移到已有 worker，缓存 selector 通过 | 慢 COM/proxy、真实 Office/PDF handler、首次 Loading→fallback 和重试 TTL 的动态体验 |
| 高对比/系统 tooltip | 已修明确自绘冲突，原生 tooltip 作为可访问性语义例外保留 | 实际自定义高对比主题、焦点、选区轮廓、文字缩放/屏幕阅读器及截图 |
| 长文本高亮 | 动态 hit metrics 与 140 行 DirectWrite 用例通过 | 真实长文件滚动、选区/匹配 clipping 和视觉确认 |

没有运行全量 selftest、全套历史 UI 测试、全量 Release 构建、安装/发布或修改用户偏好。没有暗/亮/高对比、多显示器和窄布局截图，因此不对可见 UI 做完整视觉批准。屏幕外 HWND 夹具即使通过也不能替代交互和视觉证据。

## 逐文件阅读台账

行数为本轮局部修复后工作区快照。所有条目均为全文阅读，分段工具输出如截断或缺段已经补读；没有用入口检索代替全文。首次读取的行数因新增行与当前快照有少量区别，均只涉及上述已检查的局部 diff。

| src/ui 文件 | 当前行数 | 全文阅读分段（原读取快照，闭区间） |
| --- | ---: | --- |
| advanced_search_dialog.cpp | 725 | 1–718；DPI 字体 diff 复核 |
| color_picker.cpp | 1378 | 1–1346，另补 451–487、481–841；本轮 fit/font/HC diff 复核 |
| file_operation_dialog.cpp | 1817 | 1–300、301–650、651–1000、1001–1400、1401–1814；HC 卡片 diff 复核 |
| fluent_components.cpp | 2776 | 1–450、451–900、901–1350、1351–1800、1801–2250、2251–2776；仅省略空白输出 |
| fluent_components.h | 518 | 1–518 |
| command_icons.h | 379 | 1–379 |
| fluent_menu.cpp | 1730 | 1–580、581–1160、1161–1730；修改行 diff 复核 |
| lumatext_renderer.cpp | 1169 | 1–580、581–870、871–1169；补 116–136 |
| preview_handler_host.cpp | 1240 | 1–440、441–840、841–1233；缓存查询 diff 复核 |
| quick_preview_window.cpp | 1769 | 1–400、401–800、801–1200、1201–1600、1601–1761；颜色/动态 hit ranges diff 复核 |
| ui_details_panel.cpp | 705 | 1–350、351–702；补 1–77、691–702；两处修改 diff 复核 |
| ui_pane_list.cpp | 1963 | 1–400、401–800、801–1200、1201–1600、1601–1963；仅省略空白输出 |
| ui_renderer_internal.h | 2134 | 1–530、531–1070、1071–1600、1601–2134 |
| ui_renderer.cpp | 972 | 1–480、481–972 |
| ui_renderer.h | 1285 | 1–560、561–1000、1001–1285 |
| ui_settings_view.cpp | 795 | 1–320、321–795 |
| ui_sidebar.cpp | 796 | 1–400、401–796；补 111–185、481–611 |
| window_material.cpp | 1081 | 1–550、551–1081 |

另已全文读 `menu_text_test.cpp` 与 `titlebar_layout_test.cpp` 的测试主体；局部读 panel_metrics、UI hit test、既有 selftest 的相关用例和 CMake 目标定义，用于选择独立 selector，未把这些局部依赖读取计为新的全文覆盖。
