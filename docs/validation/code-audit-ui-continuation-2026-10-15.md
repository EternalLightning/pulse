# UI 审查续篇（2026-10-15）

本轮承接 `code-audit-ui-2026-10-15.md` 的逐文件台账，保留全部既有 dirty 修改。旧台账的 28 个未全文文件中，本轮补完 9 个；仍有 19 个未全文。没有把入口扫描当作逐行审阅，也没有重跑旧轮完整 UI/preview 测试。

2026-10-16 增量：`drag_drop.cpp` 与其头文件已全文补读，并修复介质所有权/类型检查；见 `code-audit-drag-drop-2026-10-16.md`。下面表格保留本记录当时快照，新增覆盖后未全文数为 18。

## 本轮明确修复

`src/ui/preview_handler_pan.h`：拖拽已接管左键、尚未松开时隐藏或销毁预览，`Disable()` 原先立即请求卸载低级鼠标 hook；但输入线程退出条件仍等待 `suppress_left_up` 清零。真实松开事件已无法送达该 hook，会泄漏鼠标释放事件，并可能使停止中的输入线程一直留在消息等待中。

修复让 `SyncHook` 在存在待吞掉的匹配松开事件时保留 hook；释放后，停止中的线程退出，隐藏但仍存活的线程排队同步卸载 hook。没有改变普通点击、系统拖拽阈值、provider HWND 的异步滚动方式，也没有引入同步 provider 调用。

`src/bench/preview_handler_pan_test.cpp` 新增 3 个断言，用真实输入线程和替代 hook 验证隐藏时保留到松开、松开后卸载、销毁后消费松开并退出。回调状态由线程持有对象承载，避免测试超时路径捕获悬空局部变量。原有测试只在隐藏后直接调用 `ConsumeMouse`，无法发现 hook 已被卸载这一缺口。

## 实际验证

通过 `scripts\vcvars.bat` 初始化 x64 MSVC，最终测试源实际执行：

```text
cmake --build build --target pulse_preview_handler_pan_test
build\pulse_preview_handler_pan_test.exe
git diff --check -- src/ui/preview_handler_pan.h src/bench/preview_handler_pan_test.cpp
```

结果：51 PASS、0 FAIL、进程退出码 0；构建没有新增编译警告；diff whitespace 检查通过。第一次同样为 51 PASS，随后测试回调生命周期微调后复编并重跑了上述最终结果。没有运行全套 selftest、preview suite、Release 全量构建。主程序整合构建由主代理执行并在总报告记录。

**验证限度**：测试使用替代 hook 和屏幕外 HWND，不向用户桌面注入全局鼠标事件；没有实际 Office/PDF provider 拖拽录屏、暗/亮/高对比截图、DPI 或多显示器验收。不能把测试通过等同于所有实际 provider 的视觉/交互通过。旧实现的永久驻留是代码状态转换可推导的缺陷；本轮没有先在真实 provider 中制造线程泄漏。

## 旧修复核对

- `FluentTokens.h::MakeHighContrastTheme` 当前确实通过 `GetRValue/GetGValue/GetBValue` 将 COLORREF 转成 D2D RGB，避免将 BGR 编码直接交给 HexColor。这是本轮源码/diff 核对，未切换用户高对比主题。
- `view_layout.cpp::HitTest` 当前已对 `col >= columns` 及列优先布局 `row >= rows_per_column` 返回 -1，防止空白槽被折算成相邻列/行条目。本轮读了函数及 diff，没有重复旧定向测试。
- `common/utf8_file.h::DecodeUtf8Bytes` 当前在调用 UTF-8 解码前跳过 EF BB BF，且只含 BOM 时返回空串成功。本轮仅核对该函数，未修改 common，未将此局部读取计为 common 全文审查。
- 旧 UI 报告和交接工具摘录没有提供以上三项的独立测试日志证据。本报告不自行补写“旧测试已通过”；跨模块测试结果以主代理核对为准。

## 原生窗口入口与复用

本轮重新扫描 `src/ui`、`src/preview_host` 的 CreateWindow、MessageBox、TaskDialog、DialogBox、文件选择器及 ChooseColor 入口，生产窗口清单与旧报告一致。该范围仍未发现直接系统 MessageBox/TaskDialog/文件选择器入口；这不覆盖 app、shell_host 或第三方内部窗口。

`fluent_menu.cpp:891` 的 `TOOLTIPS_CLASSW` 仍是原生 Tooltip；重读 `UpdateTooltip` 确认只设置工具文本、跟踪位置和最大宽度，没有接入 Pulse 字体/主题绘制。它是可统一样式的候选，不是已截图复现的视觉缺陷。普通 `EDIT` 是输入/IME 宿主，视频子窗是媒体像素目标，preview handler 内容归第三方，不应仅因为使用原生 HWND 就机械替换。

复用证据：`ui_toolbar.cpp` 与 `ui_hit_test.cpp` 均使用 `MakeToolbarLayout` 和 `ToolbarCommandState`；settings 绘制与命中均使用 `MakeSettingsLayout`；column strip 绘制、命中使用同一 geometry；这些已存在的布局和状态函数应继续复用。对 Tooltip 的统一建议沿用既有 Painter/typography/theme，但实施前仍需验证可访问性、系统高对比、屏幕边界、长文本和 DPI。没有为减少重复而合并具有不同退出语义的 Shell/媒体/预览线程。

## 逐文件覆盖

下表列出旧台账所有 28 个未全文文件。行数为本轮结束时快照；本轮全文合计 3491 行。旧台账已全文的文件沿用旧状态，不声称本轮重读。另全文读取并修改了 `src/bench/preview_handler_pan_test.cpp`；局部核对 FluentTokens、view_layout、utf8_file 及上述 dirty diff。

| 文件 | 行数 | 本轮状态 |
| --- | ---: | --- |
| src/ui/advanced_search_dialog.cpp | 718 | 仍未全文 |
| src/ui/color_picker.cpp | 1346 | 仍未全文 |
| src/ui/command_icons.h | 379 | 仍未全文 |
| src/ui/drag_drop.cpp | 719 | 仍未全文 |
| src/ui/file_operation_dialog.cpp | 1814 | 仍未全文 |
| src/ui/fluent_components.cpp | 2776 | 仍未全文 |
| src/ui/fluent_components.h | 518 | 仍未全文 |
| src/ui/fluent_menu.cpp | 1730 | 局部 UpdateTooltip；仍未全文 |
| src/ui/fluent_menu.h | 284 | 全文 |
| src/ui/lumatext_renderer.cpp | 1169 | 仍未全文 |
| src/ui/preview_handler_host.cpp | 1233 | 仍未全文 |
| src/ui/preview_handler_pan.h | 393 | 全文 |
| src/ui/quick_preview_playback.cpp | 288 | 全文 |
| src/ui/quick_preview_window.cpp | 1761 | 仍未全文 |
| src/ui/settings_layout_sections.h | 346 | 全文 |
| src/ui/ui_column_strip.cpp | 314 | 全文 |
| src/ui/ui_details_panel.cpp | 702 | 仍未全文 |
| src/ui/ui_fluent_svg.cpp | 289 | 全文 |
| src/ui/ui_hit_test.cpp | 1044 | 全文 |
| src/ui/ui_pane_list.cpp | 1963 | 仍未全文 |
| src/ui/ui_renderer_internal.h | 2134 | 仍未全文 |
| src/ui/ui_renderer.cpp | 972 | 仍未全文 |
| src/ui/ui_renderer.h | 1285 | 仍未全文 |
| src/ui/ui_settings_core.cpp | 317 | 全文 |
| src/ui/ui_settings_view.cpp | 795 | 仍未全文 |
| src/ui/ui_sidebar.cpp | 796 | 仍未全文 |
| src/ui/ui_toolbar.cpp | 216 | 全文 |
| src/ui/window_material.cpp | 1081 | 仍未全文 |

剩余文件仍继承旧轮入口扫描/关键段落覆盖。批量输出曾被截断，已对本轮标为全文的缺失段补读；其余文件未提升覆盖声明。剩余 19 个文件、约 2.4 万行不能据此判为无缺陷；真实 provider、全局输入和完整视觉验收也仍未完成。
