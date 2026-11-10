# 交互设计参考：减少操作次数、顺应视觉习惯

这份资料回答两个问题：

1. 怎样设计能减少用户的操作次数和操作成本；
2. 怎样排版和分组才符合大多数用户的视觉扫描习惯。

每条原则都附了落点到 Pulse（文件管理器场景）的具体做法。配套的 Fluent 视觉规范见同目录的 [README.md](README.md)。

## 快速索引

| 想解决的问题 | 对应原则 | 文献 |
| --- | --- | --- |
| 一次任务要点很多下 | 交互成本、菲茨定律 | Interaction Cost、Fitts's Law |
| 菜单/工具栏选项太多，找得慢 | 希克定律、渐进披露 | Hick's Law、Progressive Disclosure |
| 用户记不住快捷键和入口 | 识别优于回忆 | Recognition and Recall |
| 用户找不到重点 | 视觉层级、F 型扫描 | Visual Hierarchy、F-Shaped Pattern |
| 分组不清、界面显乱 | 格式塔邻近原则、间距栅格 | Proximity Principle |
| 键盘用户走不通流程 | 焦点顺序、键盘交互 | Windows 键盘交互 |

## 一、减少操作次数

### 1. 交互成本（Interaction Cost）

**要点**：完成一个任务的总成本 = 视觉搜索 + 鼠标/键盘动作 + 认知负荷。减少任何一项都算优化，不必只盯着"点击次数"。

**落到 Pulse**：

- 高频路径（打开、预览、复制、移动、重命名）应能从列表直接完成，不要求先进属性页或二级菜单。
- 一个动作的入口要在视线范围内：列表行悬停按钮、右键菜单、快捷键三者至少覆盖两种。
- 减少窗口跳转：暂存盘、分栏、标签页都是"少走一趟"的设计，优先引导用户使用。

参考：[Interaction Cost: Definition](https://www.nngroup.com/articles/interaction-cost-definition/)

### 2. 菲茨定律（Fitts's Law）

**要点**：目标越大、距离越近，点击越快；屏幕边缘和角落等效于无限大的目标。

**落到 Pulse**：

- 行内操作按钮、右键菜单项的可点击区域不低于 24–28 DIP，图标小但热区要大。
- 窗口控制按钮（最小化/最大化/关闭）贴住窗口右上角，不要留无效边距。
- 拖放目标要整块高亮（整个窗格、整个侧栏项），不要只高亮文字。
- 分栏之间的拖拽手柄可以适当加宽，便于反复调整。

参考：[Fitts's Law and Its Applications in UX](https://www.nngroup.com/articles/fitts-law/)

### 3. 希克定律（Hick's Law）

**要点**：选项越多，做决定越慢；选项之间的差异越小，越难选。

**落到 Pulse**：

- 工具栏只保留最高频命令，其余收进"更多"菜单；数量控制在 6–8 个。
- 右键菜单按用途分组，常用项置顶，低频项折叠到二级菜单。
- 设置页按任务分组，避免一个页面平铺几十个开关。

参考：[Hick's Law](https://lawsofux.com/hicks-law/)

### 4. 渐进披露（Progressive Disclosure）

**要点**：先给完成主任务所需的最少信息，细节按需展开。

**落到 Pulse**：

- 高级搜索、批量重命名、冲突处理这类低频复杂功能，用独立对话框承载，只在需要时出现。
- 详情面板默认收起，预览和元数据按需展开。
- 冲突对话框提供"应用到全部"，避免重复决策。

参考：[Progressive Disclosure](https://www.nngroup.com/articles/progressive-disclosure/)

### 5. 识别优于回忆（Recognition over Recall）

**要点**：让用户看到可选项，而不是要求他记住命令、路径或快捷键。

**落到 Pulse**：

- 菜单项右侧显示对应快捷键。
- 地址栏提供历史与自动补全，面包屑可点击回跳。
- 命令入口支持搜索（如 Ctrl+K），并支持拼音/首字母匹配。
- 最近使用、常用位置放在侧栏靠前位置。

参考：[Memory Recognition and Recall in User Interfaces](https://www.nngroup.com/articles/recognition-and-recall/)

### 6. 就近操作与直接操纵

**要点**：动作放在对象旁边，优先让用户直接操作对象本身，而不是先选命令再选对象。

**落到 Pulse**：

- 重命名、删除、标签等操作在列表行内或右键菜单就近提供。
- 支持拖放移动/复制，拖拽排序侧栏和标签。
- 拖动文件时明确显示目标位置和将要执行的动作（复制/移动）。

### 7. 批量、连续与可恢复

**要点**：重复性任务要能批量完成，危险动作要能撤销或确认。

**落到 Pulse**：

- Shift 范围选择、Ctrl 追加选择、全选、反选。
- 批量重命名、批量打标签、批量移动。
- 破坏性操作给出明确后果说明，并提供撤销或回收站路径。

参考：[10 Usability Heuristics for User Interface Design](https://www.nngroup.com/articles/ten-usability-heuristics/)（第 3、5 条）

## 二、顺应视觉习惯

### 1. F 型扫描与首屏优先级

**要点**：用户阅读列表和页面时通常先横向扫顶部，再纵向扫左侧。左上角是最高价值区域。

**落到 Pulse**：

- 列表首列放名称，左侧对齐；图标、名称、扩展名的视觉权重依次递减。
- 主要操作入口放在窗口左上（导航）和右上（搜索/视图），符合扫视起点。
- 分组标题左对齐，靠间距区分层级，不依赖大色块。

参考：[F-Shaped Pattern of Reading](https://www.nngroup.com/articles/f-shaped-pattern-reading-web-content/)

### 2. 视觉层级（Visual Hierarchy）

**要点**：用尺寸、字重、颜色建立主次；同一屏幕只保留一个最强的主行动。

**落到 Pulse**：

- 标题用字重和字号区分，正文层级用 `text_secondary` 降权，避免整屏都是强调色。
- 选中态、悬停态、焦点态使用不同强度的填充，不要用同一种高亮表达三种状态。
- 强调色只用于主操作和当前选中项，其余保持中性。

参考：[Visual Hierarchy in UX: Definition](https://www.nngroup.com/articles/visual-hierarchy-ux-definition/)

### 3. 格式塔：邻近、相似、连续

**要点**：距离近的元素被看作一组；组间距必须明显大于组内间距，否则分组失效。

**落到 Pulse**：

- 工具栏图标按功能分组，组间留 16–24 DIP，组内留 4–8 DIP。
- 侧栏分区之间用间距和标题分隔，少用贯穿全宽的硬分隔线。
- 列表行内的图标、主文本、次要信息保持固定顺序和间距。

参考：[Proximity Principle in Visual Design](https://www.nngroup.com/articles/gestalt-proximity/)

### 4. 对齐与栅格

**要点**：所有间距来自同一套刻度，元素边缘对齐形成稳定的视觉节奏。

**落到 Pulse**：

- 沿用现有 4 DIP 基准（`spacing_unit`）和 8 DIP 控件圆角（`radius_control`），新控件不要引入 5、7、10 这类孤立值。
- 文本基线、图标中心、卡片内边距分别对齐，不要混用居中与顶对齐。

### 5. 状态可见性

**要点**：悬停、选中、焦点、禁用四种状态必须一眼可分，键盘焦点不能被裁剪或隐藏。

**落到 Pulse**：

- 悬停是浅填充，选中是较深填充或强调色描边，键盘焦点是外圈焦点环。
- 列表滚动容器的裁剪区域要为焦点环留出空间。
- 禁用态同时降低对比度和去掉交互反馈，不能只改颜色。

参考：[Windows 键盘交互](https://learn.microsoft.com/zh-cn/windows/apps/design/input/keyboard-interactions)

### 6. 信息密度与行高

**要点**：文件管理器需要高密度，但点击热区不能低于可用下限。

**落到 Pulse**：

- 鼠标场景行高 28 DIP 左右合适；如果未来支持触摸，触控行高需要提高到 40 DIP 以上。
- 列宽按内容自适应，数字列右对齐，文本列左对齐。
- 次要信息（日期、大小、类型）降低对比度，不与文件名争夺注意力。

### 7. 空态、加载与错误

**要点**：每个空白或异常状态都要告诉用户"现在发生了什么、下一步能做什么"。

**落到 Pulse**：

- 空文件夹区分"确实为空"和"筛选无结果"，后者提供清除筛选入口。
- 搜索中显示进度和可取消操作，不要只留一个转圈。
- 失败提示给出原因和重试/打开所在位置等动作。

## 三、Pulse 对照清单

逐项自查，命中即说明该路径的交互成本已经较低：

- [ ] 打开文件/文件夹：双击、回车、右键"打开"三种方式都可用。
- [ ] 预览：空格即可预览，不需要先打开属性或独立应用。
- [ ] 复制/移动：支持 Ctrl+C/X/V、拖放、暂存盘三种路径。
- [ ] 高频命令在工具栏可见，且不超过 8 个。
- [ ] 菜单项显示快捷键，命令入口可搜索。
- [ ] 列表行悬停时出现就近操作，不必先右键。
- [ ] 多选支持 Shift 范围、Ctrl 追加、全选。
- [ ] 破坏性操作有确认或可撤销。
- [ ] 悬停/选中/焦点/禁用四态在明暗主题下都清晰可辨。
- [ ] 组间距大于组内间距，界面不依赖分隔线也能看出分组。
- [ ] 空态和错误态都给出下一步动作。
- [ ] 所有间距取自 4/8 DIP 刻度。

## 四、参考链接（国内可直连）

**减少操作次数**

- <https://www.nngroup.com/articles/interaction-cost-definition/>
- <https://www.nngroup.com/articles/fitts-law/>
- <https://lawsofux.com/hicks-law/>
- <https://www.nngroup.com/articles/progressive-disclosure/>
- <https://www.nngroup.com/articles/recognition-and-recall/>
- <https://www.nngroup.com/articles/ten-usability-heuristics/>

**视觉习惯与排版**

- <https://www.nngroup.com/articles/f-shaped-pattern-reading-web-content/>
- <https://www.nngroup.com/articles/visual-hierarchy-ux-definition/>
- <https://www.nngroup.com/articles/gestalt-proximity/>

**Windows 交互规范（中文）**

- 命令基础知识：<https://learn.microsoft.com/zh-cn/windows/apps/design/basics/commanding-basics>
- 导航基础知识：<https://learn.microsoft.com/zh-cn/windows/apps/design/basics/navigation-basics>
- 键盘交互：<https://learn.microsoft.com/zh-cn/windows/apps/design/input/keyboard-interactions>
- Fluent 2 设计原则：<https://fluent2.microsoft.design/design-principles>

**其他设计体系**

- Material Design 3：<https://m3.material.io/>
- Apple Human Interface Guidelines：<https://developer.apple.com/design/human-interface-guidelines>
- IBM Carbon：<https://carbondesignsystem.com/>

**中文社区（观点参考，非规范）**

- 腾讯 ISUX：<https://isux.tencent.com/>
- 优设：<https://www.uisdc.com/>
- 人人都是产品经理：<https://www.woshipm.com/>

## 五、本地图片

图片位于 `assets/interaction/`，仅作本地参考（已 gitignore），版权归原作者：

| 文件 | 内容 | 来源 |
| --- | --- | --- |
| `nng-fitts-distance.png` | 距离对点击速度的影响 | NN/g |
| `nng-fitts-infinite-target.png` | 屏幕边缘作为无限大目标 | NN/g |
| `nng-fitts-padded-target.png` | 扩大热区而不放大图标 | NN/g |
| `nng-fpattern-heatmap-1.png` / `-2.png` | F 型阅读热图 | NN/g |
| `nng-gestalt-proximity-shapes.png` / `-text.png` | 邻近原则分组示例 | NN/g |
| `nng-recognition-amazon.png` | 搜索建议降低回忆负担 | NN/g |
| `ms-keyboard-focus.png` | 键盘焦点环可见性 | Microsoft Learn |
| `ms-keyboard-tab-arrow.png` | Tab 与方向键的焦点移动 | Microsoft Learn |
| `ms-navigation-header.jpg` | Windows 导航模式总览 | Microsoft Learn |
