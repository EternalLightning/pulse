# UI 与预览宿主审查记录（2026-10-15）

## 范围与覆盖程度

- 本次分工：`src/ui`、`src/preview_host`，以及直接相关的 `src/bench/dialog_close_test.cpp`。未更改 CMake。
- 入口清单覆盖上述目录全部 98 个文件：UI 91 个文件，预览宿主 7 个文件。文件数包含头文件。
- 全目录静态扫描：窗口创建、原生对话框 API、线程、消息循环、COM 初始化、GDI/句柄资源释放；阅读当前 UI diff，并检查本次 diff。
- 全文阅读：`dialog_lifecycle.h/.cpp`、`shell_icons.cpp`、`metafile_raster.cpp`、`dialog_close_test.cpp`。
- 关键段落阅读：`advanced_search_dialog.cpp`、`batch_rename_dialog.cpp`、`color_picker.cpp`、`file_operation_dialog.cpp` 的创建、嵌套消息循环和销毁；`fluent_menu.cpp` 的 Tooltip 创建/更新与退出；`shortcut_help.cpp` 的模态循环；`video_preview.cpp` 的 Open/Reset/Run；`preview_handler_host.cpp` 的处理器创建、打开、关闭和 WorkerMain；`thumbnail_cache.cpp` 的缓存、排队、取消、结果校验、共享内存、工作线程；`window_material.cpp` 的异步解码和代次校验；`ui_compositor.cpp` 的交换链、resize、present；`preview_host/main.cpp` 的 WIC/GIF 解码、缩略图、管道请求循环；`svg_raster.cpp` 的读文件、尺寸和渲染/读回。
- 其余布局、绘制、交互文件仅被入口/风险模式扫描及现有 diff 检查覆盖，**不声称 33,000 多行 UI 已逐行审阅，也不声称已完成所有视觉状态验证**。

## 已确认并修复：嵌套消息循环吞掉退出消息

触发：高级搜索、批量重命名、文件冲突确认、通用确认、颜色选择器显示期间，线程收到 `WM_QUIT`。

原行为：五处 `GetMessageW(...) > 0` 循环退出后直接清理并返回，已经取走的退出消息不会交给外层循环，主循环可能继续等待消息；退出码也丢失。现有快捷键帮助的循环已经正确重新投递，说明各弹窗的退出语义不一致。

修改：在 `src/ui/dialog_lifecycle.h` 增加 `GetModalMessage(MSG&)`，收到退出消息时立即用原退出码 `PostQuitMessage`，随后返回 false。五处入口复用此函数，普通消息仍交给各自原有的分发/键盘处理。`GetMessageW` 返回 -1 时同样停止循环，不把错误伪装成退出。

修改文件：

- `src/ui/dialog_lifecycle.h`
- `src/ui/advanced_search_dialog.cpp`
- `src/ui/batch_rename_dialog.cpp`
- `src/ui/file_operation_dialog.cpp`
- `src/ui/color_picker.cpp`
- `src/bench/dialog_close_test.cpp`

`file_operation_dialog.cpp` 已有其他未提交改动；本次只替换两处消息读取表达式，未覆盖这些修改。

## 原生窗口入口与归属

| 入口 | 当前实现 | 结论 |
| --- | --- | --- |
| `advanced_search_dialog.cpp:120`、`batch_rename_dialog.cpp:64` | Pulse 自绘顶层窗 + 编辑框 | 顶层不是遗漏美化的系统对话框 |
| `file_operation_dialog.cpp:254,604,1079` | 自绘冲突、确认、进度窗 | 已复用 compositor、Painter 和生命周期工具 |
| `color_picker.cpp:1257` | 自绘 layered popup + 编辑框 | 自绘颜色选择器，不调用系统 ChooseColor |
| `fluent_menu.cpp:335,759` | 主菜单/子菜单 layered popup | 自绘 |
| `fluent_menu.cpp:891` | `TOOLTIPS_CLASSW` 系统 Tooltip | **代码上仍是原生未统一样式入口**；此创建/更新路径没有配置 Pulse 主题字体、颜色或自绘。是否在特定系统主题下视觉突兀仍需截图确认 |
| `edit_host.h:25` | 原生 `EDIT` | 公共编辑宿主用于输入、IME/选区；上层已有颜色/字体和 LumaText 路径，不能仅因类名是 EDIT 就列为未美化弹窗 |
| `quick_preview_window.cpp:127` | Pulse 自绘快速预览顶层窗 | 自绘 |
| `shortcut_help.cpp:182` | Pulse 自绘帮助覆盖窗 | 自绘 |
| `preview_handler_host.cpp:503` | Pulse 宿主窗，承载第三方 `IPreviewHandler` | 内容由预览提供程序绘制；处理器使用 `CLSCTX_LOCAL_SERVER`，不能把任意第三方内容外观视为 Pulse 自绘遗漏 |
| `video_preview.cpp:224` | 系统媒体渲染目标子窗 | 视频像素不是普通按钮/对话框美化对象 |
| `dialog_lifecycle.cpp:280` | 点击穿透的 owner dim overlay | 辅助遮罩，不是用户交互对话框 |

本分工目录中未扫描到直接调用 `MessageBox`、`TaskDialog`、`DialogBox`、`GetOpenFileName`、`GetSaveFileName`、`ChooseColor` 的生产入口。这个结论**不覆盖** app、shell_host、IPC 或第三方组件内部创建的 UI。

## 复用与优化建议（尚未实施）

1. 将同类 app 模态循环接入新消息读取 helper，或保持已有等效 WM_QUIT 转发；已将该发现通知负责 app 的审查者。不要强行合并各弹窗的编辑框/键盘处理。
2. 菜单 Tooltip 可评估公共自绘提示组件，复用 `fluent::Painter`、typography、主题 token；需覆盖暗/亮主题、高对比度、DPI、屏幕边缘、长文本与无障碍。当前仅确认原生入口，没有截图，不直接替换为未经验证的自绘实现。
3. 窗口位置、owner 恢复、淡入和遮罩已有 `window_helpers`/`dialog_lifecycle`，新对话框应优先接入；不要继续复制模态消息循环和销毁顺序。
4. 多个异步子系统使用有界 worker slot、shared state、stop/generation gate。这些对象分别承载 Shell 图标、预览 COM、视频和图片解码，退出行为不同；没有性能证据时不为减少行数而强行统一成通用线程池。
5. `ThumbnailCache::Trim` 允许当前可见工作集超过空闲预算，代码明确以避免可见缩略图反复抖动为目的。这里不能只看到超过 16 MiB 就宣称泄漏或盲目强制裁剪。

## 实际验证

命令通过 `scripts\vcvars.bat` 初始化 x64 MSVC 后执行：

```text
cmake --build build --target pulse_dialog_close_test pulse
cmake --build build --target pulse_dialog_close_test
build\pulse_dialog_close_test.exe
git diff --check -- src/ui src/preview_host src/bench/dialog_close_test.cpp
```

- `pulse` 构建成功。
- 新测试第一次编译发现局部变量遮蔽警告，已更名修复；测试目标复编无该警告。
- `pulse_dialog_close_test.exe`：18 PASS，退出码 0。新增 5 项检查覆盖普通消息、内外两层退出和主循环接收原退出码；其余检查覆盖既有 owner/编辑窗/遮罩销毁行为。
- diff whitespace 检查通过；Git 提示部分文件后续可能 LF→CRLF，不是编译/测试错误。
- 未运行完整 selftest、全部 UI 测试或全部预览测试；未进行实际 Office/PDF/媒体提供程序功能验收，也未拍摄主题/DPI 截图。helper 回归不等于逐个实际弹窗的端到端退出已验收。

## 剩余边界

本次只实施一个已确认、可定向验证的生命周期修复。原生 Tooltip 是代码级样式差异候选，未定级为已复现视觉缺陷。其余生产代码的完整逐行阅读、全功能/视觉验收未完成；本记录不能作为“整个 UI 无 bug”的证明。

## 逐文件阅读台账（补读后）

“全文”表示已分段读完当前文件；“未读完”表示仅入口扫描或关键段阅读，不能视为通读完成。相关测试仅按实际执行和定向阅读列在正文。

| 文件 | 阅读状态 |
| --- | --- |
| src/preview_host/main.cpp | 全文 |
| src/preview_host/metafile_raster.cpp | 全文 |
| src/preview_host/metafile_raster.h | 全文 |
| src/preview_host/svg_raster.cpp | 全文 |
| src/preview_host/svg_raster.h | 全文 |
| src/preview_host/video_codec.cpp | 全文 |
| src/preview_host/video_codec.h | 全文 |
| src/ui/address_search_layout.h | 全文 |
| src/ui/advanced_search_dialog.cpp | 未读完（扫描/部分段落） |
| src/ui/advanced_search_dialog.h | 全文 |
| src/ui/batch_rename_dialog.cpp | 全文 |
| src/ui/batch_rename_dialog.h | 全文 |
| src/ui/breadcrumb.cpp | 全文 |
| src/ui/color_picker.cpp | 未读完（扫描/部分段落） |
| src/ui/color_picker.h | 全文 |
| src/ui/column_strip_layout.cpp | 全文 |
| src/ui/column_strip_layout.h | 全文 |
| src/ui/command_icons.h | 未读完（扫描/部分段落） |
| src/ui/details_column_widths.h | 全文 |
| src/ui/dialog_lifecycle.cpp | 全文 |
| src/ui/dialog_lifecycle.h | 全文 |
| src/ui/dialog_text_fit.h | 全文 |
| src/ui/drag_drop.cpp | 未读完（扫描/部分段落） |
| src/ui/drag_drop.h | 全文 |
| src/ui/edit_host.cpp | 全文 |
| src/ui/edit_host.h | 全文 |
| src/ui/empty_state_layout.h | 全文 |
| src/ui/file_operation_dialog.cpp | 未读完（扫描/部分段落） |
| src/ui/file_operation_dialog.h | 全文 |
| src/ui/fluent_components.cpp | 未读完（扫描/部分段落） |
| src/ui/fluent_components.h | 未读完（扫描/部分段落） |
| src/ui/fluent_menu.cpp | 未读完（扫描/部分段落） |
| src/ui/fluent_menu.h | 未读完（扫描/部分段落） |
| src/ui/FluentTokens.h | 全文 |
| src/ui/home_layout.h | 全文 |
| src/ui/legacy_icons.h | 全文 |
| src/ui/lumatext_renderer.cpp | 未读完（扫描/部分段落） |
| src/ui/lumatext_renderer.h | 全文 |
| src/ui/name_highlight.cpp | 全文 |
| src/ui/name_highlight.h | 全文 |
| src/ui/notification_toast.cpp | 全文 |
| src/ui/notification_toast.h | 全文 |
| src/ui/optional_lumatext.h | 全文 |
| src/ui/pane_header_icons.h | 全文 |
| src/ui/panel_metrics.h | 全文 |
| src/ui/playback_timeline.h | 全文 |
| src/ui/preview_footer_layout.h | 全文 |
| src/ui/preview_grab_cursor.h | 全文 |
| src/ui/preview_handler_host.cpp | 未读完（扫描/部分段落） |
| src/ui/preview_handler_host.h | 全文 |
| src/ui/preview_handler_pan.h | 未读完（扫描/部分段落） |
| src/ui/preview_viewport.h | 全文 |
| src/ui/quick_preview_playback.cpp | 未读完（扫描/部分段落） |
| src/ui/quick_preview_window.cpp | 未读完（扫描/部分段落） |
| src/ui/quick_preview_window.h | 全文 |
| src/ui/search_banner_layout.h | 全文 |
| src/ui/settings_layout_sections.h | 未读完（扫描/部分段落） |
| src/ui/shell_icons.cpp | 全文 |
| src/ui/shell_icons.h | 全文 |
| src/ui/shortcut_help.cpp | 全文 |
| src/ui/shortcut_help.h | 全文 |
| src/ui/sort_types.h | 全文 |
| src/ui/svg_bitmap.cpp | 全文 |
| src/ui/svg_bitmap.h | 全文 |
| src/ui/tab_shape.cpp | 全文 |
| src/ui/tab_shape.h | 全文 |
| src/ui/thumbnail_cache.cpp | 全文 |
| src/ui/thumbnail_cache.h | 全文 |
| src/ui/toolbar_command_state.h | 全文 |
| src/ui/toolbar_layout.h | 全文 |
| src/ui/typography.cpp | 全文 |
| src/ui/typography.h | 全文 |
| src/ui/ui_address_search.cpp | 全文 |
| src/ui/ui_column_strip.cpp | 未读完（扫描/部分段落） |
| src/ui/ui_compositor.cpp | 全文 |
| src/ui/ui_compositor.h | 全文 |
| src/ui/ui_details_panel.cpp | 未读完（扫描/部分段落） |
| src/ui/ui_fluent_svg.cpp | 未读完（扫描/部分段落） |
| src/ui/ui_hit_test.cpp | 未读完（扫描/部分段落） |
| src/ui/ui_home.cpp | 全文 |
| src/ui/ui_pane_header_icons.cpp | 全文 |
| src/ui/ui_pane_list.cpp | 未读完（扫描/部分段落） |
| src/ui/ui_renderer_internal.h | 未读完（扫描/部分段落） |
| src/ui/ui_renderer.cpp | 未读完（扫描/部分段落） |
| src/ui/ui_renderer.h | 未读完（扫描/部分段落） |
| src/ui/ui_settings_context.cpp | 全文 |
| src/ui/ui_settings_core.cpp | 未读完（扫描/部分段落） |
| src/ui/ui_settings_view.cpp | 未读完（扫描/部分段落） |
| src/ui/ui_sidebar.cpp | 未读完（扫描/部分段落） |
| src/ui/ui_toolbar.cpp | 未读完（扫描/部分段落） |
| src/ui/video_preview.cpp | 全文 |
| src/ui/video_preview.h | 全文 |
| src/ui/view_layout.cpp | 全文 |
| src/ui/view_layout.h | 全文 |
| src/ui/window_helpers.cpp | 全文 |
| src/ui/window_helpers.h | 全文 |
| src/ui/window_material.cpp | 未读完（扫描/部分段落） |
| src/ui/window_material.h | 全文 |
