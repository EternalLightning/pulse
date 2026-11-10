# UX 设计参考总览

[interaction-design.md](interaction-design.md) 深挖了"减少操作次数"和"视觉习惯"两个问题；本文是全景地图，覆盖交互效率之外的反馈、错误、信息架构、搜索、列表密度、空状态、一致性、无障碍、国际化、动效和用户研究。

所有链接均已验证国内可直连（2026-11-10）。

## 维度地图

| 维度 | 解决什么问题 | 首读资料 |
| --- | --- | --- |
| 交互效率 | 操作次数多、路径长 | Interaction Cost、Fitts's Law |
| 反馈与等待 | 用户不知道系统在做什么 | Website Response Times、Doherty Threshold |
| 错误与恢复 | 出错后无法挽回、提示无用 | Error-Message Guidelines |
| 信息架构 | 找不到入口、层级混乱 | NN/g IA、Jakob's Law |
| 搜索与筛选 | 搜不到、结果不可控 | NN/g Search、Windows 输入与交互 |
| 列表与密度 | 信息过密或过疏 | Windows 布局和间距、Fluent 2 Layout |
| 空状态与引导 | 空白页不知道下一步 | TDesign/Arco Empty、渐进披露 |
| 一致性 | 同一操作不同表现 | Consistency Heuristic、Fluent 2 原则 |
| 无障碍 | 键盘、读屏、对比度不可用 | WCAG 2.2、Windows 辅助功能 |
| 国际化 | 文案变长、语言方向、字体 | Microsoft 全球化、W3C i18n |
| 动效与感知性能 | 卡顿感、转场迷失方向 | Windows 运动、Progress Indicators |
| 用户研究 | 凭感觉改版，无法验证 | Usability Testing 101、启发式评估 |

## 一、交互效率

核心原则见 [interaction-design.md](interaction-design.md)：交互成本、菲茨定律、希克定律、渐进披露、识别优于回忆、就近操作、批量与可恢复。

补充资料：

- <https://www.nngroup.com/articles/interaction-cost-definition/>
- <https://www.nngroup.com/articles/fitts-law/>
- <https://lawsofux.com/hicks-law/>

## 二、反馈、等待与感知性能

**原则**

- 0.1 秒内响应视为"即时"；1 秒内保持思路连续；10 秒是注意力的上限。
- 超过约 400 ms 的操作需要明确的进行中反馈。
- 等待要有进度、可取消、可离场（转到后台继续）。

**资料**

- Website Response Times：<https://www.nngroup.com/articles/website-response-times/>
- Progress Indicators：<https://www.nngroup.com/articles/progress-indicators/>
- Doherty Threshold：<https://lawsofux.com/doherty-threshold/>
- Windows 命令基础知识：<https://learn.microsoft.com/zh-cn/windows/apps/design/basics/commanding-basics>

**Pulse 落点**

- 目录枚举、缩略图、索引搜索的等待要分级：小于 1 秒不打断；1–10 秒给进度；超长任务进操作中心并可继续浏览。
- 批量操作显示总数、已完成数和当前项，而不是只有一个转圈。
- 失败后保留已完成的部分，不整批回滚到未知状态。

## 三、错误预防与恢复

**原则**

- 错误消息说明三件事：发生了什么、为什么、下一步能做什么。
- 能预防的错误不要用确认框兜底（禁用不可用操作、提前校验）。
- 破坏性操作提供撤销、回收站或二次确认，三者按风险选择。

**资料**

- Error-Message Guidelines：<https://www.nngroup.com/articles/error-message-guidelines/>
- 10 Usability Heuristics（第 5、9 条）：<https://www.nngroup.com/articles/ten-usability-heuristics/>
- 本地示例：`assets/interaction/nng-error-clear.jpg`、`assets/interaction/nng-error-txtdot.jpg`

**Pulse 落点**

- 权限拒绝、路径过长、文件名非法、目标已存在等错误分别给出可执行动作，不复用同一句"操作失败"。
- 批量冲突对话框显示来源、目标和将执行的决策，并提供"应用到全部"。
- 删除走回收站；覆盖、移动等高风险动作明确说明后果。

## 四、信息架构与导航

**原则**

- 导航回答三个问题：我在哪、我能去哪、怎么回去。
- 层级深度优先于广度会拖慢查找，文件管理器通常采用"侧栏 + 面包屑 + 标签"的混合结构。
- 遵循用户在其他应用里形成的习惯，降低迁移成本。

**资料**

- NN/g 信息架构专题：<https://www.nngroup.com/topic/information-architecture/>
- NN/g 导航专题：<https://www.nngroup.com/topic/navigation/>
- Windows 导航基础知识：<https://learn.microsoft.com/zh-cn/windows/apps/design/basics/navigation-basics>
- Jakob's Law：<https://lawsofux.com/jakobs-law/>

**Pulse 落点**

- 侧栏承载固定位置和分组，面包屑承载当前位置，标签页承载并行任务，三者职责不重叠。
- 保持 Explorer 的核心习惯：双击打开、F2 重命名、Delete 删除、Ctrl+Z 撤销。
- 平铺/列表/详细信息等视图切换不改变排序与位置记忆。

## 五、搜索与筛选

**原则**

- 搜索框要让用户知道搜索范围：当前文件夹、当前盘还是全盘。
- 自动补全和搜索建议减少回忆负担；零结果要给出放宽条件的入口。
- 筛选条件必须可见、可清除，避免用户忘记自己处于筛选状态。

**资料**

- NN/g 搜索专题：<https://www.nngroup.com/topic/search/>
- Windows 输入和交互：<https://learn.microsoft.com/zh-cn/windows/apps/design/input/>

**Pulse 落点**

- 搜索范围在输入框附近可见，切换范围不需要清空重输。
- 零结果页提供"清除筛选""改为全盘搜索""按内容搜索"等下一步。
- 拼音、首字母和多音字匹配的结果要有命中高亮（已有实现，见列表绘制）。

## 六、列表、表格与信息密度

**原则**

- 高密度界面依靠对齐和分组维持可读性，而不是靠分隔线。
- 数字右对齐、文本左对齐、日期使用统一格式。
- 列宽、排序、分组状态要可记忆。

**资料**

- Windows 布局和间距：<https://learn.microsoft.com/zh-cn/windows/apps/design/basics/content-basics>
- Fluent 2 布局：<https://fluent2.microsoft.design/layout>
- Fluent 2 版式：<https://fluent2.microsoft.design/typography>

**Pulse 落点**

- 名称列固定在最左，数字列右对齐，长文本截断时保留扩展名。
- 行高与点击热区分离：鼠标行高 28 DIP，热区可更大。
- 斑马纹、大小比例条这类辅助信息降低对比度，不与文件名竞争。

## 七、空状态与首次使用

**原则**

- 空状态区分三种情况：确实为空、筛选无结果、加载失败。
- 每种空状态都给出一个主要动作，而不是只写"暂无数据"。
- 首次使用通过就地提示引导，不强制教程。

**资料**

- TDesign Empty 组件示例：<https://tdesign.tencent.com/vue/components/empty>
- Arco Design Empty 组件示例：<https://arco.design/vue/component/empty>
- 渐进披露：<https://www.nngroup.com/articles/progressive-disclosure/>

**Pulse 落点**

- 空文件夹提示"拖入文件或右键新建"；筛选无结果提示"清除筛选"；加载失败提示"重试"。
- 暂存盘为空时说明用途和操作方式（拖入文件、集中复制/移动）。
- 首次打开时的引导只覆盖高频路径，不展示完整手册。

## 八、一致性与平台习惯

**原则**

- 同一操作在应用内保持相同的入口、文案和反馈。
- 自定义外观可以改变视觉，但不应改变用户已形成的操作习惯。
- 平台规范优先于个人偏好，除非有明确收益。

**资料**

- NN/g 一致性启发式：<https://www.nngroup.com/articles/consistency-and-standards/>
- Jakob's Law：<https://lawsofux.com/jakobs-law/>
- Fluent 2 设计原则：<https://fluent2.microsoft.design/design-principles>

**Pulse 落点**

- 所有列表使用同一套多选、右键、拖放和快捷键规则，不因视图模式改变。
- 自定义 Fluent 外观下仍保留 Windows 的键位与菜单语义。
- 新增命令先检查是否已有入口，避免同一功能出现三种叫法。

## 九、可访问性

**原则**

- 键盘可达：所有功能不依赖鼠标，焦点顺序符合阅读顺序，焦点环始终可见。
- 对比度：正文至少 4.5:1，大文本 3:1，非文本控件 3:1。
- 信息不能只靠颜色传达（选中、标签、错误都要有第二种线索）。
- 尊重系统设置：高对比度、减少动画、文本缩放。

**资料**

- WCAG 2.2 快速参考：<https://www.w3.org/WAI/WCAG22/quickref/>
- Windows 辅助功能：<https://learn.microsoft.com/zh-cn/windows/apps/design/accessibility/accessibility>
- Windows 键盘交互：<https://learn.microsoft.com/zh-cn/windows/apps/design/input/keyboard-interactions>

**Pulse 落点**

- 当前自绘控件没有 UIA 实现，读屏和自动化工具无法识别，这是最大的无障碍缺口。
- 高对比度主题已覆盖（`IsHighContrast()`），需要继续保证焦点环和选中态在四态下可辨。
- 表格类控件补全键盘导航：方向键移动、Home/End、PageUp/PageDown、类型筛选。

## 十、国际化与本地化

**原则**

- 文案长度按 30%–50% 膨胀预留空间，避免按钮截断。
- 布局支持从右到左（RTL）语言；图标和方向性隐喻需要镜像。
- 日期、数字、排序规则遵循区域设置；CJK 字体需要单独配置字号和行高。

**资料**

- Microsoft 全球化和本地化：<https://learn.microsoft.com/zh-cn/windows/apps/design/globalizing/globalizing-portal>
- W3C Internationalization：<https://www.w3.org/International/>

**Pulse 落点**

- 列宽和按钮宽度用测量而不是固定像素，避免英文长单词溢出。
- 文件名排序使用区域感知的比较规则，避免中英文混排顺序异常。
- 界面语言切换后重新测量文本缓存（已有 `InvalidateCaches`）。

## 十一、动效与感知性能

**原则**

- 动效解释空间关系：元素从哪里来、到哪里去，方向要一致。
- 时长跟随距离和复杂度，避免所有过渡使用同一时长。
- 用户开启"减少动画"时降级为淡入淡出或直接切换。
- 加载占位应保持布局稳定，避免内容到达后跳动。

**资料**

- Windows 运动：<https://learn.microsoft.com/zh-cn/windows/apps/design/signature-experiences/motion>
- Fluent 2 动效：<https://fluent2.microsoft.design/motion>
- Doherty Threshold：<https://lawsofux.com/doherty-threshold/>

**Pulse 落点**

- 分栏、标签、详情面板的转场方向与用户操作方向一致。
- 已读取 `SPI_GETCLIENTAREAANIMATION` 的窗口保持一致，其余动画入口也应接入。
- 滚动和视图切换避免布局跳动，缩略图先占位再替换。

## 十二、用户研究与验证

**原则**

- 5 名目标用户就能发现大部分严重可用性问题。
- 启发式评估适合自查，可用性测试适合验证真实任务。
- 用任务完成率、完成时间、错误次数、求助次数衡量改动，而不是只靠主观感受。

**资料**

- Usability Testing 101：<https://www.nngroup.com/articles/usability-testing-101/>
- 10 Usability Heuristics：<https://www.nngroup.com/articles/ten-usability-heuristics/>

**量化操作成本**

- 用 KLM（Keystroke-Level Model）估算：按键约 0.20 s、指向约 1.10 s、心理准备约 1.35 s。
- 例如"选中文件 → 右键 → 打开"大约需要 1 次指向 + 1 次点击 + 1 次菜单选择，可直接和"双击打开"对比。
- 改动前后各测一次同一任务，能量化"减少操作次数"的实际收益。

## 中文设计体系（可直接参考交互规范）

- TDesign 设计指南：<https://tdesign.tencent.com/design/>
- Arco Design 规范：<https://arco.design/docs/spec/introduce>
- Ant Design 设计规范：<https://ant.design/docs/spec/introduce-cn>（页面为单页应用，浏览器打开后加载）

这些体系里的"反馈""效率""一致性""可控"章节，和本文的分类可以互相对照。

## 怎么使用这份资料

1. 新功能立项时，从"维度地图"里选 2–3 个最相关的维度。
2. 先读对应原则，再对照 Pulse 现状写下差异。
3. 用 [interaction-design.md](interaction-design.md) 末尾的自查清单过一遍。
4. 上线前做一次 5 人可用性测试，记录任务完成时间与错误次数。
