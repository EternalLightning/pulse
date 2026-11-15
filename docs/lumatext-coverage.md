# LumaText 覆盖现状与迁移建议

核对日期：2026-11-15，范围为当前工作树的 `src/ui`、`src/app` 及公共渲染入口。下列迁移已在当前工作树实施；构建、运行与截图检查结果见第 6 节，不表示安装版或发布版已更新。通用视觉规则见 [UI 与文字渲染规范](ui-style-guide.md)。

## 1. 建议结论

**建议让程序拥有绘制权的普通 UI 文字，在 Auto 模式下优先经统一入口使用 LumaText。** 保留 DirectWrite 排版、测量、缺字回退和命中；逐步减少无意绕过公共入口的直接绘制。

不要求每一段可见文字都由 LumaText 栅格化：系统标题栏、系统对话框、外部预览提供器、彩色字形策略、高对比度、显式渲染模式和实际失败回退各有边界。原生 EDIT 的交互职责也继续保留。

统一的是默认呈现策略、入口和例外规则，而不是让所有字号、字重、轮廓增重都相同。已确认的标题 D 样式只用于标题；正文仍沿用原有配置。

## 2. 现有管线

| 入口 | Auto 模式下的行为 | 使用方式 |
| --- | --- | --- |
| `fluent::Painter::DrawText` / `DrawTextWithBrush` | 优先 `Compositor::DrawLumaText`，失败后 DirectWrite | 普通按钮、标签、说明等简单文字 |
| `Compositor::DrawTextLayout` | 优先 LumaText 绘制已有的 DirectWrite layout；保留回退 | 多行、选区命中、范围着色等已经拥有 layout 的文字 |
| `Compositor::DrawDialogTitle` | DirectWrite layout + LumaText，无额外轮廓增重 | 所有程序自绘弹窗标题；当前也用于操作中心和快速预览标题 |
| `PresentChildEdit` 与 LumaText 编辑呈现 | 原生 EDIT 处理输入，自绘层呈现；真实失败保留原生回退 | 地址、查找、重命名、通用输入等已有宿主 |
| `MainRenderer::DrawTextRect`、文件名和标签助手 | 多数先尝试 LumaText，再回退 | 主界面大量普通文字已有覆盖 |
| `Painter::DrawTextNative` | 直接 DirectWrite，不尝试 LumaText | 显式旁路；用于设置页大标题的 Sharp/Smooth 路径，以及共享标题的末级失败路径 |
| 直接 `dc->DrawText*` / `target->DrawText*` | 取决于外围分支；可能是正常回退，也可能是未迁移 | 必须查看调用链，不能按 API 名称统计成缺陷 |

`LumaTextEnabled()` 还受运行库、Auto 模式、高对比度等条件控制。编译开启 `PULSE_WITH_LUMATEXT` 不等于每一帧、每个控件都实际使用了 LumaText。

已有布局优先走 `Compositor::DrawTextLayout`，避免为了换栅格器重新建立另一套 shaping、换行、选区或命中逻辑。无布局的短文字继续使用 `Painter` 或 `DrawLumaText`。公共标题使用自己的 D 样式入口。

## 3. 已有覆盖，不应重复迁移的部分

- 公共按钮、普通字段外观、标签、菜单、导航、徽标、InfoBar 的大量文字，经 `Painter` 进入自动模式策略。
- 主界面 `DrawTextRect`、单行省略、标签标题、文件名的许多分支已经先调用 LumaText；它们后面的 DirectWrite 调用是必要回退。
- 详情面板、设置的多行说明、部分空状态、搜索高亮文件名等已经通过 `Compositor::DrawTextLayout`。
- 快速预览的状态、缩放提示、文件页脚和播放信息，多数已经先尝试 LumaText；不要只看到后面的 `DrawTextW` 就再次包装。
- 输入内容在 Auto 模式优先使用已有 LumaText layout 路径；高对比度或运行库不可用可回退。隐藏期误触发原生回退的问题已修复。

以上表示代码具备对应分支，不保证任意机器、字体和错误状态都会成功使用该分支。验收应结合实际文字、后端计数、回退状态和截图。

## 4. 本轮迁移结果

| 状态 | 位置与内容 | 当前情况 | 保留的契约与验收重点 |
| --- | --- | --- | --- |
| 已迁移 | [operation_center.cpp](../src/ui/operation_center.cpp)，`OperationCenter::Text` | 所有任务文字复用原 layout，经 `Compositor::DrawTextLayout` | 保留任务字重、换行、省略与透明卡片合成，检查长路径和滚动 |
| 已迁移 | [conflict_details.h](../src/ui/conflict_details.h)，`DrawConflictText` | 单行与换行统一经公共 layout 入口 | 保留双栏/单栏、长路径、末行与伸出边界 |
| 已迁移 | [file_operation_dialog.cpp](../src/ui/file_operation_dialog.cpp)，`DrawWrappedText` | 多行正文经公共 layout 入口；相邻已覆盖的单行助手保持原路径 | 保留确认、错误详情、滚动全文、页脚与窄窗口布局 |
| 已迁移 | [ui_pane_list.cpp](../src/ui/ui_pane_list.cpp)，`DrawPaneEmptyState` 的 `pane.is_changes` 说明 | 居中的多行 layout 经公共入口 | 保留居中、实际高度及其他空状态 |
| 已迁移 | [ui_settings_view.cpp](../src/ui/ui_settings_view.cpp)，设置页大标题 | Auto 经 `Painter::DrawText`；显式模式仍使用 `DrawTextNative` | 保持 22 DIP / 400 Display，不套弹窗 D 样式；保留 DirectWrite OUTLINE 失败回退及绘制状态恢复 |
| 已迁移 | [global_search_window.cpp](../src/app/global_search_window.cpp)，`Label`、多行范围 Tooltip | 普通及高亮标签、多行提示经公共 layout 入口 | 标签在 `96 * scale` DPI 下用 DIP；Tooltip 在 96 DPI 下用物理像素并恢复 DPI；Fluent 图标及测试用离屏 WIC 目标保留 DirectWrite |
| 已统一入口，保留 DirectWrite 栅格化 | [quick_preview_window.cpp](../src/ui/quick_preview_window.cpp)，`DrawTextPreview` | 原 layout 经公共入口，仍传入 `ENABLE_COLOR_FONT` | 保留彩色字形边界，因此此处不计为实际 LumaText 覆盖；选择、查找、滚动和状态栏视口保持原逻辑 |
| 已迁移 | [text_diff_window.cpp](../src/ui/text_diff_window.cpp)，正文行绘制 | 原等宽 layout 经公共入口 | 保留列对齐、差异范围背景和命中；比较长行、双栏/统一模式与滚动成本 |
| 已迁移 | [ui_pane_list.cpp](../src/ui/ui_pane_list.cpp)，`DrawCenteredIconName` | 原多行文件名 layout 经公共入口 | 保留测量、截断预算、范围着色、视图布局和重命名字段 |

表中是本轮处理的具体路径，不是对所有未来调用点的封闭清单。静态调用次数不代表运行时覆盖率；公共入口仍按用户模式、高对比度、彩色字形和实际失败决定后端。

## 5. 明确保留的边界

| 边界 | 当前处理与理由 |
| --- | --- |
| Windows 系统标题栏、原生文件选择/属性/UAC 等 | 程序没有统一的文字绘制权；不为更换栅格器重写系统交互 |
| 文本比较的非客户区顶栏 | 当前为 `WS_OVERLAPPEDWINDOW`，由 Windows 绘制；内部正文另行评估 |
| 外部 Shell / Office / 文档等预览提供器 | 内容由提供器拥有；程序自己的标题、控件仍遵守公共规则 |
| 原生 EDIT 输入行为 | 保留 IME、剪贴板、选区、撤销等，继续通过宿主自绘呈现 |
| 高对比度、Sharp/Smooth 模式、LumaText 不可用或绘制失败 | 保留 DirectWrite/原生回退，不能强制绕开用户和系统策略 |
| `ENABLE_COLOR_FONT` 路径 | 当前 `Compositor::DrawTextLayout` 明确保留 DirectWrite 的彩色字形路径；未证明等价前不更换该策略 |
| Fluent 图标、向量勾叉、SVG 插画 | 图形的视觉规则独立于正文；继续使用现有图标与向量入口，不把所有图标重新当普通文字处理 |

保留这些边界不代表所有普通 UI 旁路都有必要。任何新增 `DrawTextNative` 或直接 `DrawText*` 路径应注明用途，防止以后把“历史遗漏”误认为设计决定。

## 6. 迁移实施与验证

本轮不更换 shaping、换行、选区或命中模型，也不增加另一套布局缓存。设置页标题保持原来的显示字体层级，单独提供 22 DIP 小样；正文使用原来的光学增重，弹窗标题 D 样式保持不变。

定向验证入口如下（仅运行相应筛选，不默认运行完整套件）：

| 目标与筛选 | 验证范围 |
| --- | --- |
| `pulse_operation_center_ui_test --text-rendering` | 任务与冲突单行/多行原 layout 的实际 LumaText 字形、无意外回退、Sharp/Smooth、四档 DPI 与浅深主题 |
| `pulse_operation_center_ui_test --smooth-scroll` | 实际操作中心卡片滚动、长路径与透明合成 |
| `pulse_batch_conflict_dialog_test --scroll` | 冲突双栏/窄窗口、长路径与滚动 |
| `pulse_file_operation_failure_test --scroll-only` | 超长错误全文的滚动边界和窗口交互 |
| `pulse_file_operation_failure_test --popup-style-only` | 实际错误详情、末行与页脚边界，浅深主题、中英文、100%/150% DPI 和高对比度 |
| `pulse_text_render_settings_test --migration-only` | 22 DIP 小样与真实设置页、变更空状态、多行文件名、截断、命中与重命名几何；四档 DPI、浅深主题、Auto/Sharp/Smooth |
| `pulse_global_search_window_test --text-render-only` | 标签、高亮、省略、多行 Tooltip 的实际后端与 DPI 恢复；四档 DPI、浅深主题、显式模式和高对比度回退 |
| `pulse_text_diff_ui_test --text-rendering` | 长行正文实际绘制计数、双栏/统一模式、选择、命中、双向滚动、模式与高对比度回退；两档 DPI 和浅深主题 |
| `pulse_audit_ui_remaining_test --preview-style-only` | 预览原布局、复杂中文、末行、正文与状态栏边界 |
| `pulse_global_search_window_test --appearance-keyboard` | 预览主题切换保留选择、查找、滚动与文档状态；正文末尾可达且页脚不选中文字 |

文件名用例记录固定 60 帧的进程 CPU 时间和 wall time，文本比较用例记录单帧首次、重复和滚动 wall time；这些是本机场景采样，不代表冷启动、实际屏幕帧率或整体性能提升。原尺寸图片和本轮结果保存在本地 `bench_data/text-migration-20261115/verification.md`，本地日志/图片不保证随仓库分发。

本轮主程序与相关测试目标构建通过，以上 10 个定向筛选全部通过（最终合计 1564 项断言），并检查了代表性原尺寸截图。采样显示 LumaText 的一致呈现有额外成本：24 个多行文件名约 1.43 ms/帧（Smooth 对照约 0.37 ms/帧）；长行双栏比较滚动约 19.81–21.60 ms/帧（Sharp 对照约 7.35–9.69 ms/帧）。后者超过 16.7 ms 的单帧预算，不能据此承诺稳定 60 FPS。没有运行完整历史回归或更新安装版。

使用 `LumaTextStats` 的字形和回退计数辅助判断。编辑是否实际原生回退还要检查宿主状态，不能只凭 `edit_directwrite_draws` 的名字下结论，它也统计共用 DirectWrite layout 的编辑绘制过程。

## 7. 后续核对入口

```powershell
# 先定位入口，再看是否已有 LumaText 分支；搜索结果数量不是覆盖率。
rg -n -g '*.cpp' -g '*.h' 'DrawLumaText|DrawDialogTitle|DrawTextNative|DrawTextLayout' src/ui src/app
rg -n -g '*.cpp' -g '*.h' -- '->DrawText(W|Layout)?\(' src/ui src/app
```

运行时策略见 [typography.h](../src/ui/typography.h) 与 [ui_compositor.cpp](../src/ui/ui_compositor.cpp)，SDK 与构建配置见[运行库与 SDK 文档](static-runtime-packaging.md)。本文没有给出按静态调用次数计算的“LumaText 百分比”，因为回退、图标、文本量和实际运行频率会使这种数字失真。
