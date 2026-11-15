# Fluent 设计参考

本目录收集 Windows 11 / Fluent 2 官方设计资料，供 Pulse 的 Fluent 风格自绘 UI 对照使用。所有资源均从国内可直连的微软站点获取，未使用代理，也未从 GitHub 下载。

## 目录

| 路径 | 内容 | 来源 |
| --- | --- | --- |
| `assets/windows11/` | Windows 11 设计规范配图（中文本地化站点） | Microsoft Learn |
| `assets/fluent2/` | Fluent 2 设计系统插图 | fluent2.microsoft.design |
| `assets/interaction/` | 交互成本、菲茨定律、F 型扫描等示意图 | NN/g、Microsoft Learn |

## 专题文档

- [Pulse UI 与文字渲染规范](../ui-style-guide.md)：项目已确认的颜色、字体、控件、弹窗、材质与验收规则。
- [LumaText 覆盖与迁移建议](../lumatext-coverage.md)：当前渲染入口、迁移结果、保留边界与定向验证。
- [interaction-design.md](interaction-design.md)：交互成本、菲茨定律、视觉层级、F 型扫描等，含 Pulse 自查清单。
- [ux-reference.md](ux-reference.md)：12 个 UX 维度的全景索引（反馈、错误、信息架构、搜索、列表密度、空状态、一致性、无障碍、国际化、动效、用户研究）。

`assets/` 中的图片版权归 Microsoft 所有，仅作本地设计参考，不随仓库分发（已加入 `.gitignore`）。如需对外发布截图，请自行确认授权或重绘。

## Windows 11 参考图

| 文件 | 用途 | 对应实现 |
| --- | --- | --- |
| `mica-header.png` / `mica-light.png` / `mica-dark.png` | 云母材质的明暗表现与窗口层次 | `src/ui/window_material.cpp`、`theme.surface_*` |
| `acrylic-light.png` / `acrylic-dark.png` / `acrylic-menu.png` | 亚克力底色、菜单浮出层材质 | `WindowMaterial`、`fluent_menu.cpp` |
| `color-hero.png` / `color-light-controls.png` / `color-dark-controls.png` | 明暗主题下的控件配色与交互状态 | `src/ui/FluentTokens.h` 的 `Theme` 令牌 |
| `typography-metrics.svg` | Segoe UI 字形度量与基线 | `typography.cpp`、`typography.h` |
| `spacing-buttons.png` | 按钮间距与 8 DIP 栅格 | `spacing_unit`、`radius_control` 等几何令牌 |
| `icons-command-bar.png` | 命令栏图标风格与尺寸 | `assets/fluent_*.svg`、`command_icons.h` |
| `motion-minimize-restore.png` / `motion-page-transitions.png` | 最小化还原、页面过渡的运动节奏 | `fluent_components.h` 的 `motion` 常量、`tab_indicator_animation.h` |

## Fluent 2 参考图

| 文件 | 对应主题 |
| --- | --- |
| `principles-focus.webp` / `principles-natural.webp` / `principles-oneforall.webp` | 设计原则 |
| `color-hero.webp` / `color-neutral.webp` / `color-interaction-states.webp` | 色彩层级、中性色阶、交互状态 |
| `typography-alignment.webp` | 文字对齐 |
| `motion-main.webp` | 动效总览 |
| `elevation-hero.webp` / `elevation-low-ramp.webp` | 高度/阴影层级 |
| `iconography.webp` | 图标体系 |
| `layout-grid.webp` | 布局栅格 |

## 在线文档（国内可直连）

Windows 11 设计中心：

- <https://learn.microsoft.com/zh-cn/windows/apps/design/>
- 云母材质：<https://learn.microsoft.com/zh-cn/windows/apps/design/style/mica>
- 亚克力：<https://learn.microsoft.com/zh-cn/windows/apps/design/style/acrylic>
- 颜色：<https://learn.microsoft.com/zh-cn/windows/apps/design/signature-experiences/color>
- 版式：<https://learn.microsoft.com/zh-cn/windows/apps/design/signature-experiences/typography>
- 运动：<https://learn.microsoft.com/zh-cn/windows/apps/design/signature-experiences/motion>
- 图标：<https://learn.microsoft.com/zh-cn/windows/apps/develop/ui/controls/icons>
- 布局和间距：<https://learn.microsoft.com/zh-cn/windows/apps/design/basics/content-basics>

Fluent 2 设计系统：

- 首页：<https://fluent2.microsoft.design/>
- 设计原则：<https://fluent2.microsoft.design/design-principles>
- 颜色：<https://fluent2.microsoft.design/color>
- 版式：<https://fluent2.microsoft.design/typography>
- 动效：<https://fluent2.microsoft.design/motion>
- 高度：<https://fluent2.microsoft.design/elevation>
- 材质：<https://fluent2.microsoft.design/materials>
- 图标：<https://fluent2.microsoft.design/iconography>
- 布局：<https://fluent2.microsoft.design/layout>

WinUI 3 Gallery 包含全部 WinUI 控件的可交互示例，可对比控件行为与动画细节。源码在 `microsoft/WinUI-Gallery`（GitHub 直连较慢，仅作可选参考）。

## 建议阅读顺序

1. 设计原则 → 明确层级与留白取向。
2. 颜色 + 云母/亚克力 → 对照 `FluentTokens.h` 与 `window_material.cpp`。
3. 版式 + 间距 + 高度 → 对照 `typography.*`、几何令牌、`theme.surface_*`。
4. 动效 → 对照 `fluent_components.h` 的 `motion` 常量与各类过渡实现。
5. 图标 → 对照 `assets/fluent_*.svg` 与 `command_icons.h`。
