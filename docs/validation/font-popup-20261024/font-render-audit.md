# LumaText 可见文字路径审查（2026-10-24）

## 判定口径

本次从当前工作区未提交修改与实际产品绘制调用开始检查。`CreateTextLayout`、宽度/换行测量、命中测试仍使用 DirectWrite，并不等于文字仍由 DirectWrite 绘制。

当前设置的真实含义是：Auto 对受支持的普通 UI 文字优先使用 LumaText；Sharp / Smooth 使用 DirectWrite；高对比度和 LumaText 不支持或失败的情形保留 DirectWrite 回退。不能把 Auto 描述为所有窗口、所有文字已经完全使用 LumaText。

## 本次补齐的普通文字

| 位置 / 符号 | 原问题 | 补齐路径 |
| --- | --- | --- |
| `src/ui/ui_renderer.cpp` — `BreadcrumbLayout`，`src/ui/ui_toolbar.cpp` — 面包屑/地址回退文字 | 段宽和省略仅按 DirectWrite 测量，绘制显式 native | 两处测量改走 `MeasureLayoutText`，两处绘制移除 native 强制标志；绘制、HitTest 和 drop target 继续共用同一段布局 |
| `src/ui/ui_renderer_internal.h` — `DrawTextEndEllipsis` | 状态栏、分组标题、搜索提示、路径列、详情/列视图、暂存卡片等调用统一绕过 LumaText | 显式传入 owning compositor 和当前背景，优先 `DrawLumaText`，保留原 DirectWrite 省略回退 |
| 同文件 — `DrawTabTitle` | 短标签、长标签以及保留扩展名的标签始终 DirectWrite | 整串、截断 stem 和扩展名均尝试 LumaText；宽度用共享 `MeasureLayoutText`，避免用较窄的旧测量裁掉末字 |
| `src/ui/fluent_components.cpp` — `DrawSidebarItem` / `DrawSidebarSectionHeader` / `DrawDriveSidebarItem` | 名称、计数、详情和分区标题强制调用 `DrawTextNative`；驱动名称直接绘制 layout | 普通文字改走现有 `Painter::DrawText`；驱动名称先尝试 LumaText，失败保留原省略 layout |
| `src/ui/ui_sidebar.cpp` — 底部“设置” | 普通文字强制 native | 改走 `Painter::DrawText` |
| `src/ui/ui_settings_view.cpp` — 页面大标题 | 对 layout 设置字号/字重后直接绘制 | 单独创建原字号/字重的 Display format，交给 `Painter::DrawText` |
| `src/ui/ui_details_panel.cpp` — 快捷操作行按钮文字 | 单行居中标签直接绘制 layout | LumaText 优先，原私有 layout 回退仍保留 |
| `src/ui/file_operation_dialog.cpp` — `DrawEllipsizedText` | 进度窗口来源/目标/当前文件和确认框单行文字直接绘制 layout | 传入当前背景，LumaText 优先 |
| `src/ui/batch_conflict_dialog.cpp` — `Text` | 单行冲突摘要/列标题直接绘制 layout | 仅 `NO_WRAP` 分支尝试 LumaText；用 layout 继承显式 near 段落对齐 |
| `src/ui/operation_center.cpp` — `Text` | 统计标签/值等普通小字直接绘制 layout | 仅 `small_text && !wrap` 尝试 LumaText，避免遗漏 layout 范围字重和多行约束 |
| `src/ui/quick_preview_window.cpp` / `quick_preview_playback.cpp` | 标题、图像 HUD、页脚、空/错误/加载状态、播放按钮和播放信息直接 DrawText | 这些普通文字增加 LumaText 优先路径；预览正文和图标保持原路径 |
| `src/ui/text_diff_window.cpp` — `text_at` | 比较窗普通标签/行号/页脚直接 DrawText | 增加 LumaText 优先路径；diff 正文的布局/高亮/选择保持原路径 |
| `src/ui/shortcut_help.cpp` — text lambda | 快捷键说明普通文字直接 DrawText | 增加 LumaText 优先路径 |

补齐普通 top/bottom 对齐文字时发现 `LumaTextRenderer::DrawOn` 原来固定垂直居中。本次使其遵守 `IDWriteTextFormat::GetParagraphAlignment()` 的 near / center / far，并把该属性加入绘制面缓存键，防止同文字/尺寸的不同段落位置错误复用缓存。未改写 LumaText 换行、富文本或命中引擎。

定向执行还复现了 Auto 下混合中英文长状态文字回退：真实 `build/lumatext.dll` 的未限宽布局报告 `453.359375`，设置 `max_width=186` 和 `LT_TEXT_ELLIPSIS_END` 后仍报告同一宽度。原代码因此拒绝该布局并走 DirectWrite。现保留原生省略优先，仅在其不能满足单行/宽度约束时，用 Luma 自己的 `lt_text_layout_hit_test_position` 簇起点对 `prefix+…` 做二分测量，生成实际可容纳的 Luma 布局；任何 API 失败仍保留原回退。没有放宽超宽断言或直接裁切末字。定向绘制用组合字符和当前雅黑字体实际支持的补充平面 U+20164 检查实际保留前缀没有拆开组合字符或 UTF-16 代理对。

这里没有扩大 emoji 字体支持。生产 cascade 仅包含 Segoe UI Variable 与微软雅黑，且 `allow_system_fallback=false`。源码没有通用的“禁止代理对”判定：使用相同配置直接调用真实 DLL，U+20164 创建布局成功且索引 0/1 都命中 `(text_position=0,text_length=2)`；U+1F600 在初始 `lt_text_layout_create` 就返回 `LT_E_FONT_UNAVAILABLE (-7)`，尚未进入本次簇补偿。最初把 emoji 当作 Luma 成功用例造成四个 Auto 失败，现将该错误预期改为明确的 unsupported 边界断言：Auto 的 Luma 成功绘制计数不增长、失败回退计数增长，Sharp/Smooth 不尝试 Luma，实际文字仍由原 DirectWrite 路径绘制。不能把此项描述为“emoji 已迁移成功”。

## 仍由旧绘制路径呈现的实际可见文字

下列项仍是用户可见的旧绘制，不应以“只剩测量”概括。本次为保持现有排版、交互和修改范围，没有直接替换。

| 位置 / 符号 | 实际内容 | 保留原因及继续迁移需要的验证 |
| --- | --- | --- |
| `src/ui/lumatext_renderer.cpp` — 配置 cascade 未覆盖的字形 | 例如 U+1F600 emoji | 真实 DLL 返回字体不可用，保留 DirectWrite 字体 fallback；这是受支持范围边界，本次不新增字体或改变 cascade 系统 |
| `src/app/global_search_window.cpp` — `Label` | 浮动全局搜索普通标签和搜索匹配着色结果 | 此窗口用 `96*scale` 的 DIP render target，现 LumaText renderer 以物理像素/96 DPI录制；简单替换会产生缩放错误。需统一坐标系并保持范围着色 |
| 同文件 — 多行目录 tooltip | 长路径换行 tooltip 正文 | 多行布局仍为 DirectWrite；单行 tooltip 已有 Painter 路径 |
| `src/ui/fluent_components.cpp` — `DrawWrappedCaption` | 设置项等换行说明 | 当前 LumaText `Draw` 是单行、自动尾省略 API；没有等价的多行布局/高度协议 |
| 同文件 — `DrawButton` 的 `wrap_text` 分支、`DrawInfoBar` 的 stacked message | 窄按钮多行标签、信息栏换行说明 | 必须保持原行高、换行、居中及容器高度；不能把全文塞进单行自动省略 API |
| `src/ui/ui_settings_core.cpp` — label lambda 的多行 description | 较高说明区的设置卡片描述 | 同上，需多行绘制与现有高度测量一致 |
| `src/ui/ui_pane_list.cpp` — `DrawPaneEmptyState` 的 changes 分支和 `ChangeBannerLayout` 绘制 | 更改视图空状态/说明横幅 | 当前是多行 DirectWrite layout |
| 同文件 — 文件名着色 layout（`DrawNameHighlightBackground` 附近） | 搜索匹配文件名、启用扩展名调暗时的文件名 | 一次 shape 后进行范围着色与匹配背景定位；拆成几段 Luma 单行会破坏字距/末字墨迹和高亮坐标。普通无样式文件名已有 Luma 路径 |
| 同文件 — `DrawCenteredIconName` | 图标视图居中多行文件名 | 包含多行、尾省略、匹配背景和截断状态判定，需要同一布局的测量与绘制 |
| `src/ui/ui_sidebar.cpp` — `cutLabel` badge layout | 暂存卡片“剪切”等小徽标 | 在 layout 上应用范围 bold，不能直接复用原普通 format，否则字重改变 |
| `src/ui/file_operation_dialog.cpp` — `DrawWrappedText` | 确认/错误内容及多行详情 | 多行布局同上 |
| `src/ui/batch_conflict_dialog.cpp` — `Text` 的 wrap 分支 | 两行或更多路径/冲突说明 | 保留原换行和行数约束 |
| `src/ui/operation_center.cpp` — `Text` 的非 small 和 wrap 分支 | 任务/面板半粗标题、换行错误 | 标题在 layout 上设置全范围 semibold；错误为多行。迁移须保留字重与换行 |
| `src/ui/ui_details_panel.cpp` — `details_text_layout_` | 侧边文本/十六进制预览正文 | 多行、滚动和等宽内容；本次只迁移普通 UI 标签 |
| `src/ui/quick_preview_window.cpp` — `DrawTextPreview` | 文本/十六进制预览正文 | 搜索命中、鼠标选择、滚动和选区背景与同一 DirectWrite layout 配套 |
| `src/ui/text_diff_window.cpp` — 正文 `DrawTextLayout` | diff 文本内容 | 范围匹配、文本选择、命中测试、滚动与 layout 配套 |
| `src/ui/lumatext_renderer.cpp` — EDIT 的 `visible_layout` 绘制 | 自绘输入框、选区和组合输入文字 | 代码已明确使用 DirectWrite 来保证绘制/命中几何一致。Luma 类名不代表输入文字经过 Luma glyph renderer |
| `src/app/archive_dialog.cpp` — `DrawTextW` | 压缩/密码框的标题、提示和 owner-draw 控件文字 | 这是产品自绘 GDI 文字，确实尚未迁移；没有现成 compositor。需连同原生 HWND 对话框的绘制生命周期单独迁移 |
| `src/app/new_item_dialog.cpp` — `DrawTextW` | 新建项目标题、提示、错误、按钮和空输入 cue | 同上，是产品自绘 GDI 路径，不应误称系统原生文字 |
| `src/app/settings_value_dialog.cpp` — `DrawTextW` | 设置值对话框标题及 owner-draw 按钮 | 同上 |

`Painter::DrawTextNative` 的定义仍保留，但本次复查 `src/` 已没有实际产品调用。图标字体的 glyph、系统 MessageBox/TaskDialog、第三方 Shell/preview HWND 不属于普通文本迁移；不能因出现 DirectWrite/GDI API 名字就机械替换。

## 定向验证入口

`src/bench/text_render_settings_test.cpp` 新增 `--plain-text-only`，不会进入原完整设置/偏好用例矩阵：

```powershell
cmake --build build --target pulse pulse_text_render_settings_test
.\build\pulse_text_render_settings_test.exe --plain-text-only
```

此分支使用真实 compositor/Painter 和共享 helper，逐项检查 Auto 的实际 Luma draw 计数增长、Sharp/Smooth 计数不增长；覆盖短/长状态、短/长 tab 及扩展名、侧栏项目/分区/驱动、near/center/far、组合字符和受支持补充平面字形的省略边界，以及未覆盖 emoji 的真实回退。段落对齐用直接 compositor 绘制、失败后用同一 format 的 DirectWrite 绘制，验证 `DrawOn` 的契约；不使用始终垂直居中的 Painter 来测试 near/far。另验证本地、UNC、压缩包路径在 620/1100 DIP 下的段边界、尾段路径、实际省略及真实 HitTest 的 index/path/tooltip。通过同帧其它内容不变、只切换 address_editing 的前后 Luma 计数差，验证实际 MainRenderer 面包屑绘制。生成 100%/150%、明暗、三模式共 12 张普通文字截图和 12 张面包屑截图至本目录 `plain-text/`。

当前执行环境中，原生测试子进程在 compositor 初始化前就无法用 `CreateFileW` 写证据目录（错误 5），不能归因字体初始化或绘制。默认截图仍调用产品 `Compositor::SaveSnapshot`。父 harness 显式设置 `PULSE_TEST_STREAM_SNAPSHOTS=1` 时，仅定向分支使用 `src/bench/snapshot_output.h` 将真实 GPU 帧经 WIC 内存编码为 PNG，再通过 stdout 传给已获目录写权限的父进程保存；定向子进程此时不创建目录。未修改 ACL、产品截图接口或原完整矩阵截图路径。

统计断言能证明路径选择，不能独立证明视觉质量。仍需查看截图确认省略号、扩展名、末字、对齐、侧栏间距。本分支也不声称已实际打开验证每一种操作弹窗、播放窗口或上述保留的复杂文本路径。构建/运行及视觉结果由主代理统一补录，本文没有预先标记通过。
