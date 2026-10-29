# 待确认 issue 修复选择清单

整理日期：2026-10-23。基于本地已保存的 issue 原文、核验结果与修复记录整理，本次没有重新抓取 GitHub 状态。共 **106 条**：缺少条件 66 条、定向未复现 10 条、已有针对实现但未运行验收 27 条、行为已确认但缺陷未定 3 条。

## 勾选与处理约定

- 将需要处理的条目前面的 `- [ ]` 改成 `- [x]`，也可使用编辑器的复选框。**勾选表示“这条需要修复”，不表示已经修复，也不表示已确认本地存在同类 bug。**
- 保存后告诉我“读取勾选项”。我会重新读取这份文件，只按勾选项确定本轮处理范围。
- 我会先阅读对应 issue 的故障描述、已有证据和当前本地实现，再向你询问确认问题所必需的信息；缺少的信息会按关联场景合并询问。
- 你可以在“你的补充”处填写现象、步骤或期望，也可以先留空，等我阅读后再问。无需一次准备所有条目的材料。
- 沿用此前要求：确认本地存在同类问题后才实施修复。尚未确认时先补充信息和验证，不仅凭勾选就修改代码。
- 混合 issue 仅处理其中的故障部分，不展开建议。本清单中的“尚需核实”沿用审计时的条件记录，不代表现在仍禁止执行相应验证；具体操作按届时确认的范围进行。

完整审计、已确认问题及修复结果仍见 [统一报告](E:/Projects/pulse/docs/issue-bug-audit-2026-10-23.md)。本文件用于选择和补充信息。

## 本次选择补充（2026-10-23）

- #290 为用户误勾选，按用户回复取消选择，不纳入本次修复。
- #314、#308：按用户要求排除所有可移动设备和 USB 外接存储（包括系统识别为固定磁盘的外置硬盘/SSD）的持久索引，保留当前目录临时扫描搜索。此前真实 G: exFAT USB 后端验证 29 项通过；本次未检测到在线 exFAT 可移动设备，仅补充无设备和过期 generation 边界，不将历史实盘结果重标为本次硬件验收，也不宣称原持久索引故障已复现或修复。
- #302：已停止读写并清理派生分卷查询副本，采用 v12 差分 varint 倒排编码及精确 UTF-16 basename 共享。最终现场目录逻辑字节从 2,335,240,520 B 降至 568,018,992 B，累计减少约 75.68%；保留 aggregate A/B、真实 WAL 等恢复数据，重建后的第 204 代名称池仍约 73.9 MB。维护期间节点和快照正常变化，该差值不是冻结数据集的纯压缩率，也不能承诺达到 Everything 的容量。验证、回滚及部署状态以统一报告为准。
- #374：在自有隔离目录创建 8 TiB 逻辑尺寸、0 字节分配的稀疏文件，真实回收后端返回 `COPYENGINE_E_RECYCLE_SIZE_TOO_BIG (0x80270037)`；生产操作管理器经 Shell IPC 进入确切根的永久删除确认，拒绝后任务取消、原目录保留，6 项检查通过。此前注入确认/接受/取消的 8 项也通过；测试链未提交永久删除，末尾仅核验身份与标记后清理自有 fixture，未清空用户回收站。原系统文件夹权限及现场返回值仍未验收，未据此确认或修复原故障。
- 微信“在文件夹中显示”：当前实验性 Explorer 接管开关仍关闭，用户配置哈希未变。设置入口及隔离选择交付已验证，其中生产选择/确认链 9 项通过；未操作用户微信或 Explorer，真实微信菜单及安装版完整接管仍未验收。
- 新增问题及明确改动：已修复重命名拖选的可见状态反复变化、Filter Enter/Esc 字符漏入原生提示音路径，并把已注册的“以管理员身份运行”移到最外层“打开”之后；具体定向验证及硬件/实际声音限制见统一报告。安装文件替换、服务状态、回滚及现场验收结论统一记录在[完整报告](E:/Projects/pulse/docs/issue-bug-audit-2026-10-23.md)。
- 以上复选框表示处理选择；实际结论、验证结果和未解决项统一记录在完整审计报告中。

## 缺少条件（66 条）

缺少原环境、样本或步骤，尚不能确认本地存在同类问题。

- [ ] [#373 Windows系统下“选择其他应用”窗口不在前台显示](https://github.com/jimmgreen/pulse/issues/373)
  - 尚需核实：需用户桌面上的应用选择器实测及前台窗口/进程记录；本次禁止执行真实 Shell 动作，未打开选择器。
  - 你的补充：

- [ ] [#370 ”在终端中打开“功能无法自动聚焦到终端窗口](https://github.com/jimmgreen/pulse/issues/370)
  - 尚需核实：需同版本系统终端扩展、真实菜单调用与前台焦点轨迹；本次未执行终端命令。
  - 你的补充：

- [ ] [#368 在文字渲染选择清晰时，英文会有明显的横向位移问题，导致字母间距有明显偏差](https://github.com/jimmgreen/pulse/issues/368)
  - 尚需核实：需原字体、12px参数、清晰选项对应上游实现与同场参考截图；默认渲染图不能验证该模式。
  - 你的补充：

- [ ] [#361 对于.exe文件，右键添加到开始始终不显示](https://github.com/jimmgreen/pulse/issues/361)
  - 尚需核实：需对同一未固定 exe 比较原生/Pulse COM 菜单及个人固定状态；静态动词无 Pin 不足以确认缺项。
  - 你的补充：

- [ ] [#348 程序内图标和任务栏图标不一样](https://github.com/jimmgreen/pulse/issues/348)
  - 尚需核实：缺少具体两处图标及原始版本/缓存条件；未操控任务栏或图标缓存。
  - 你的补充：

- [ ] [#340 名字太长的重命名显示有问题](https://github.com/jimmgreen/pulse/issues/340)
  - 尚需核实：原故障主要为图片，没有等价文字步骤；单行首尾滚动已测，未获得原图标布局触发条件。
  - 你的补充：

- [ ] [#339 我的下载文件夹已更改路径，Pulse不支持](https://github.com/jimmgreen/pulse/issues/339)
  - 尚需核实：需要报告者已迁移 Downloads 的 KnownFolder 实际返回值、目标路径、侧栏缓存与隐藏状态；未迁移本机 KnownFolder 或偏好进行试验。
  - 你的补充：

- [ ] [#338 右键菜单不生效，右键EXE没有固定到开始菜单](https://github.com/jimmgreen/pulse/issues/338)
  - 尚需核实：需 exe 是否已固定的状态、完整 COM 菜单及 Send to 子菜单；原评论中出现的是取消固定，不能混同固定项缺失。
  - 你的补充：

- [ ] [#333 win10 Pulse-1.0.56-portable版本的窗口效果切换无效](https://github.com/jimmgreen/pulse/issues/333)
  - 尚需核实：环境为26300/26H2，非Windows10；需真实Win10材质切换。
  - 你的补充：

- [ ] [#330 BUG：Windows系统重命名光标会跳到首位](https://github.com/jimmgreen/pulse/issues/330)
  - 尚需核实：需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。
  - 你的补充：

- [ ] [#327 修改系统右键后，不能正常展示](https://github.com/jimmgreen/pulse/issues/327)
  - 尚需核实：需具体勾选扩展、目标文件类型和一次菜单 query 的返回/partial 日志；未把共性缓存风险认定为本条根因。
  - 你的补充：

- [ ] [#321 触摸适配不太好](https://github.com/jimmgreen/pulse/issues/321)
  - 尚需核实：需要真实触摸硬件/驱动与文件区手势输入，记录是否触发鼠标提升及 OLE 拖动；未改变系统输入设置。
  - 你的补充：

- [ ] [#319 三屏情况下，桌面有的地方点击不了。](https://github.com/jimmgreen/pulse/issues/319)
  - 尚需核实：缺三屏拓扑、原 Win10 环境和桌面实际命中/z-order 记录；不能用单屏隐藏窗口替代。
  - 你的补充：

- [ ] [#318 好像没有自动刷新呢](https://github.com/jimmgreen/pulse/issues/318)
  - 尚需核实：原报告指定 Hide Folders；本次没有该产品/过滤驱动及版本，普通 SetFileAttributes 通过不能替代 Hide Folders 的真实事件行为验证。
  - 你的补充：

- [ ] [#317 命名文件文字顺序错乱！](https://github.com/jimmgreen/pulse/issues/317)
  - 尚需核实：需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。
  - 你的补充：

- [ ] [#316 文件名重全名光标串位](https://github.com/jimmgreen/pulse/issues/316)
  - 尚需核实：需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。
  - 你的补充：

- [x] [#314 exFAT 分区显示索引数量增加，但搜索不到任何文件](https://github.com/jimmgreen/pulse/issues/314)
  - 尚需核实：需要可写 exFAT 卷、隔离索引配置和可追溯计数来源，分别检查卷根添加、子目录添加与搜索；本次未创建/格式化卷或更改真实索引服务。
  - 你的补充：

- [x] [#308 exFAT volume shows index count increase but search returns no results](https://github.com/jimmgreen/pulse/issues/308)
  - 尚需核实：需要 #314 同样的 exFAT 卷、根配置和查询/计数日志，不能用 NTFS 搜索结果替代。
  - 你的补充：

- [x] [#302 索引文件太大，1.7GB，Everything 索引才 136MB，另一个包括内容索引才 50MB](https://github.com/jimmgreen/pulse/issues/302)
  - 尚需核实：需要同一文件集合、名字/内容索引配置、各数据库文件大小与表/分片去重统计。
  - 你的补充：

- [ ] [#298 第三方软件无法换醒](https://github.com/jimmgreen/pulse/issues/298)
  - 尚需核实：需真实 .lnk 的目标/关联/固定状态和完整菜单；上传文件对话框接管不纳入本次 bug 结论。
  - 你的补充：

- [ ] [#292 右键文件夹不显示 Light Image Resizer（文件正常）](https://github.com/jimmgreen/pulse/issues/292)
  - 尚需核实：未建立该扩展版本安装条件；需同文件/文件夹菜单对照及层级返回，不能仅由通用裁剪风险断定同因。
  - 你的补充：

- [ ] [#290 选择某一个文件夹标签的时候就闪退](https://github.com/jimmgreen/pulse/issues/290)
  - 尚需核实：需要触发目录的隔离样本/会话状态与崩溃转储，再对原入口复测；不猜测缩略图、Shell 或索引为根因。
  - 你的补充：

- [ ] [#283 使用bug](https://github.com/jimmgreen/pulse/issues/283)
  - 尚需核实：第1项固定宽度仅为行为事实，不足以确认缺陷；第2项外部复制和暂存托盘优先级、第3项真实U盘删除未在UI组运行验收。
  - 你的补充：

- [ ] [#279 [BUG] 添加网络位置时 即使选择好了搜索索引文件夹 任弹出并无法添加网络位置](https://github.com/jimmgreen/pulse/issues/279)
  - 尚需核实：需要隔离配置下的真实服务器共享/映射网络盘，实测选择框最近位置、确认后能否继续添加；不能把本地目录被拒绝等同于原循环弹框已复现。
  - 你的补充：

- [ ] [#278 打开方式选择其他应用的选择框没在前台打开](https://github.com/jimmgreen/pulse/issues/278)
  - 尚需核实：需原系统应用选择器、实际 foreground owner 和任务栏状态；本次未执行真实 Shell 打开方式动作。
  - 你的补充：

- [ ] [#275 占位盘符影响索引生成](https://github.com/jimmgreen/pulse/issues/275)
  - 尚需核实：需要 Sangfor/对应 subst 盘、普通用户与索引服务账户 QueryDosDevice/卷枚举和配置错误码；未接受权限推测为根因。
  - 你的补充：

- [ ] [#274 右键项目文件夹，选择Git Commit后，会卡很长时间才能出现git提交窗口](https://github.com/jimmgreen/pulse/issues/274)
  - 尚需核实：需相同 Git Shell 扩展版本、仓库状态及 query/invoke/进程就绪分段计时；本次不调用 Git Commit。
  - 你的补充：

- [ ] [#272 预览动画存在异常](https://github.com/jimmgreen/pulse/issues/272)
  - 尚需核实：原视频逐帧对照；可复现的预览开关顺序和动画捕获
  - 你的补充：

- [ ] [#270 pulse接管后，在打开pulse窗口的情况下，点击任务栏的资源管理器和Win+E还是会忙碌弹窗，而不是直接切到pulse。](https://github.com/jimmgreen/pulse/issues/270)
  - 尚需核实：需已授权的系统接管、Win+E 与 Explorer 启动进程/窗口时间线；本次没有改变接管或发送系统打开动作。
  - 你的补充：

- [ ] [#266 预览经常失效](https://github.com/jimmgreen/pulse/issues/266)
  - 尚需核实：原文件类型、HRESULT、宿主及provider前后状态；本轮私有IPC失败不能代替原问题复现
  - 你的补充：

- [ ] [#265 【bug】关闭窗口后win+E再打开会显示多窗口](https://github.com/jimmgreen/pulse/issues/265)
  - 尚需核实：需受控的多窗口会话文件、启动偏好及完整关闭/重新打开步骤；本次不读写用户会话或改变启动偏好。
  - 你的补充：

- [ ] [#262 小问题](https://github.com/jimmgreen/pulse/issues/262)
  - 尚需核实：原安装布局和宿主版本/文件；错误码和安全软件信息；本轮环境失败不判定为产品缺陷
  - 你的补充：

- [ ] [#251 打开后，无法退出移动硬盘，桌面的文件夹也没有办法在桌面上删除](https://github.com/jimmgreen/pulse/issues/251)
  - 尚需核实：安全弹出需真实可移动设备、句柄/索引状态与取消移除恢复流程；桌面问题需隔离 Shell 回收删除原父子目录场景。仅保留预释放实现缺口，不从缺分支直接断言设备必然不可弹出。
  - 你的补充：

- [ ] [#246 右键菜单问题](https://github.com/jimmgreen/pulse/issues/246)
  - 尚需核实：需卸载前后实际扩展注册/设置目录快照及 Notepad++ 安装类型；未将保留 seen 机制直接认定为当前 UI 一定残留。
  - 你的补充：

- [ ] [#241 Listary不会自动跳转的问题](https://github.com/jimmgreen/pulse/issues/241)
  - 尚需核实：需 Listary 的实际版本、跳转入口/适配配置和当前窗口识别记录；现有材料不能判断 Pulse 或 Listary 谁负责。
  - 你的补充：

- [ ] [#237 nas映射的磁盘，文件无法删除](https://github.com/jimmgreen/pulse/issues/237)
  - 尚需核实：需相同 NAS 设备/映射协议/权限及隔离测试文件的删除返回；本次未删除网络映射盘数据。
  - 你的补充：

- [ ] [#234 重命名鼠标跳转问题](https://github.com/jimmgreen/pulse/issues/234)
  - 尚需核实：需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。 实际鼠标指针本身是否跳动仍需明确步骤。
  - 你的补充：

- [ ] [#230 用 Pulse 代替文件资源管理器的开关都打开了了，win+E打开的是Windows资源管理器，不知道是不是Windows预览版的原因，26340.9616](https://github.com/jimmgreen/pulse/issues/230)
  - 尚需核实：需26340.9616及实际接管注册/Win+E链路；本次未更改注册表或系统接管，不能确认是否为 OS 回归。
  - 你的补充：

- [ ] [#228 [Bug] 设置为默认文件管理器后，桌面双击文件夹错误触发第三方程序](https://github.com/jimmgreen/pulse/issues/228)
  - 尚需核实：需原 HKCU/HKLM Directory 默认动作、隐藏标记、关联合并和 Procmon 重放；本次未临时禁用 AnyCode、修改注册表或启动桌面默认动作。
  - 你的补充：

- [ ] [#226 无法访问服务器](https://github.com/jimmgreen/pulse/issues/226)
  - 尚需核实：需要可控的要求认证 SMB 服务器、全新凭据会话、UNC 状态与错误码；未把缺交互入口静态迹象扩大成已复现全部服务器访问失败。
  - 你的补充：

- [ ] [#214 右键还是没有winrar](https://github.com/jimmgreen/pulse/issues/214)
  - 尚需核实：需报告者 WinRAR/7-Zip 实际版本、传统/MSIX安装类型和文件夹菜单；本次未安装或调用压缩扩展。
  - 你的补充：

- [ ] [#212 手势软件功能失效](https://github.com/jimmgreen/pulse/issues/212)
  - 尚需核实：需Wgestures实际版本和真实手势注入环境；未安装/修改软件或用模拟消息声称兼容。
  - 你的补充：

- [ ] [#211 重命名文件的时候，录入文字的过程中，光标会自动往前跳](https://github.com/jimmgreen/pulse/issues/211)
  - 尚需核实：需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。
  - 你的补充：

- [ ] [#205 对 Bandzip 右键菜单不支持](https://github.com/jimmgreen/pulse/issues/205)
  - 尚需核实：需 Bandizip 具体版本/安装类型、目标文件类型及完整原生/Pulse菜单对照；未执行扩展动作。
  - 你的补充：

- [ ] [#200 新建文件(夹)后不能自动选中重命名](https://github.com/jimmgreen/pulse/issues/200)
  - 尚需核实：需要报告者实际入口（新建按钮/菜单/Shell 模板）、文件类型与期望命名时序；本次未操作新建对话框，不能把 UI 设计差异判成选择失效。
  - 你的补充：

- [ ] [#199 关于无论咋样都更新不了新版本的问题](https://github.com/jimmgreen/pulse/issues/199)
  - 尚需核实：需要失败时下载/替换日志、旧版本原始包、占用文件与可写权限，在隔离更新目录复测卡顿和失败；不从当前版本号推测原因。
  - 你的补充：

- [ ] [#184 [BUG反馈] 选中 .srt 文件时右侧详细信息侧边栏异常遮挡其他界面元素](https://github.com/jimmgreen/pulse/issues/184)
  - 尚需核实：真实srt关联provider；打开交互面板后的重叠和输入验证；不能由overlay设计单独推出原故障
  - 你的补充：

- [ ] [#176 右键菜单设置与实际显示不一致](https://github.com/jimmgreen/pulse/issues/176)
  - 尚需核实：需具体勾选项目、handler注册、返回菜单与设置数据的同一时点快照；本次只读源码，没有改用户配置。
  - 你的补充：

- [ ] [#175 右键git菜单经常被自动关掉](https://github.com/jimmgreen/pulse/issues/175)
  - 尚需核实：需菜单打开到关闭的激活/焦点/异步完成时间线和 Git 扩展状态；本次未打开真实 Git 菜单或模拟系统失焦。
  - 你的补充：

- [ ] [#165 我的还是不能右键打开终端呀,可以用自带的,不能使用pulse的](https://github.com/jimmgreen/pulse/issues/165)
  - 尚需核实：需原会话环境、内置菜单实际启动错误/HRESULT与alias可执行行为；本次未打开终端，不把alias存在当启动成功。
  - 你的补充：

- [ ] [#162 [BUG]重命名时首次输入时输入焦点丢失且选词后光标移动到最左侧；[建议]预览文件时按照真实文件头进行判定。](https://github.com/jimmgreen/pulse/issues/162)
  - 尚需核实：需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。 大GIF失败需原样本和预览宿主验证，UI组未宣称修复。
  - 你的补充：

- [ ] [#154 新版本中，zip文件右键时找不到解压缩选项](https://github.com/jimmgreen/pulse/issues/154)
  - 尚需核实：需 NanaZip版本及zip完整菜单，不能用本机静态动词缺 Extract 或维护者Bandizip测试宣称复现/解决。
  - 你的补充：

- [ ] [#142 图片预览有卡顿，且大图片预览不显示](https://github.com/jimmgreen/pulse/issues/142)
  - 尚需核实：原图尺寸/编码/位深；解码错误、内存和延迟测量；未用普通小图片代替原件
  - 你的补充：

- [ ] [#140 对图片文件右键没有打印按钮](https://github.com/jimmgreen/pulse/issues/140)
  - 尚需核实：需原图片关联、png/报告图片类型以及完整COM菜单对照；本次未调用打印。
  - 你的补充：

- [ ] [#138 暗色设置单选按钮选中文字看不清](https://github.com/jimmgreen/pulse/issues/138)
  - 尚需核实：原文指定Win10暗色单选框；当前非Win10环境，普通深色目录截图不代替设置单选控件验收。
  - 你的补充：

- [ ] [#135 删除文件BUG及空格预览视频文件bug](https://github.com/jimmgreen/pulse/issues/135)
  - 尚需核实：TF介质与删除原错误码；原卡顿视频及播放测量；本组未做TF删除
  - 你的补充：

- [ ] [#122 索引服务无法安装](https://github.com/jimmgreen/pulse/issues/122)
  - 尚需核实：需要原版本/升级残留、服务账户和权限、端点/进程版本与安装错误码，在隔离可回滚环境复现；拒绝访问原因目前未定。
  - 你的补充：

- [ ] [#116 使用中文重命名文件时，光标会出现在最前面，导致输入的字符顺序有问题。](https://github.com/jimmgreen/pulse/issues/116)
  - 尚需核实：需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。
  - 你的补充：

- [ ] [#115 wps打开文件位置功能不工作，提示“Windows无法访问指定设备、路径或文件。你可能没有适当的权限访问该项目。”](https://github.com/jimmgreen/pulse/issues/115)
  - 尚需核实：需同版本WPS、原路径与 Shell调用/权限记录；本次未启动WPS或改变系统关联。
  - 你的补充：

- [ ] [#92 OGG格式无法预览，及音乐相关问题](https://github.com/jimmgreen/pulse/issues/92)
  - 尚需核实：Ogg内部编码与实际文件、系统codec；播放与磁盘容量刷新运行验证；不将评论修复宣称当成本地实现
  - 你的补充：

- [ ] [#81 设为默认文件资源管理器后的问题](https://github.com/jimmgreen/pulse/issues/81)
  - 尚需核实：需开启/取消前后的用户关联快照、This PC盘符默认动作和真实Shell失败信息；本次不修改系统接管/注册表。
  - 你的补充：

- [ ] [#52 [BUG] 标题栏窗口按钮图标重复绘制且错位重叠（最小化/最大化/关闭）](https://github.com/jimmgreen/pulse/issues/52)
  - 尚需核实：原问题为DWM原生按钮透过材质叠影；需要真实标准样式、DWM材质和悬停截图。离屏自绘按钮正常不能宣布原bug已修复或已复现。
  - 你的补充：

- [ ] [#51 大码流的视频几乎都没有办法播放，能不能调用 Quick Look？](https://github.com/jimmgreen/pulse/issues/51)
  - 尚需核实：原始片段和系统codec；真实播放失败码与帧率；普通视频成功不能结案
  - 你的补充：

- [ ] [#50 [Bug] 拖拽窗体不能触发Groupy](https://github.com/jimmgreen/pulse/issues/50)
  - 尚需核实：需真实Groupy版本、窗口过滤与拖动合并；未安装软件或使用全局输入。
  - 你的补充：

- [ ] [#20 窗体显示有点异常](https://github.com/jimmgreen/pulse/issues/20)
  - 尚需核实：系统强调色标题栏开关与Mica/DWM实际组合未切换；离屏像素不含系统DWM底层着色。
  - 你的补充：

- [ ] [#19 excel预览卡顿](https://github.com/jimmgreen/pulse/issues/19)
  - 尚需核实：复杂原工作簿；真实provider及滚动/拖动测试；不得由早期评论或微小XLSX泛测关闭后续报告
  - 你的补充：

## 定向未复现（10 条）

已做限定范围的检查，但没有复现原报告；仍需要原场景信息。

- [ ] [#359 [Bug] 重命名：移动光标，文字不移动，超出看不到](https://github.com/jimmgreen/pulse/issues/359)
  - 尚需核实：仅单行编辑器横向滚动；本地未实现评论里的六行折行浮卡，未覆盖真实F2图标视图/键盘自动重复/IME。
  - 你的补充：

- [ ] [#354 超大、大图标模式一些图标很小在左下角](https://github.com/jimmgreen/pulse/issues/354)
  - 尚需核实：原问题的具体图标与实际Large/ExtraLarge UI截图；本例只出现小图案，未复现左下角偏移
  - 你的补充：

- [ ] [#343 [BUG] 自定义的文件夹图标也会重绘](https://github.com/jimmgreen/pulse/issues/343)
  - 尚需核实：真实IPC/UI及原自定义provider未验收；未逐像素对照原附件
  - 你的补充：

- [ ] [#320 [bug] 选中的文件被修改后，选中状态会变成其他文件](https://github.com/jimmgreen/pulse/issues/320)
  - 尚需核实：仅覆盖普通目录、单选和大小重排；mtime/分组/多选/内容搜索等其他路径未验证。原报告没有给出确切排序与入口，不据此宣布所有情形修复。
  - 你的补充：

- [ ] [#312 深色模式下，状态栏字体颜色显示异常](https://github.com/jimmgreen/pulse/issues/312)
  - 尚需核实：离屏生产渲染、默认主题/强调色；未复现用户整机材质、原3000px窗口与完整索引/搜索状态。
  - 你的补充：

- [ ] [#280 [BUG] 右键菜单无法显示固定到快速访问](https://github.com/jimmgreen/pulse/issues/280)
  - 尚需核实：仅验证默认内存设置下的菜单构建，没有打开真实菜单、执行固定或写入偏好；原报告个人 item_enabled 和选择对象仍未复测。
  - 你的补充：

- [ ] [#260 点击刷新没有反应](https://github.com/jimmgreen/pulse/issues/260)
  - 尚需核实：原文仅“点击刷新没有反应”，缺具体视图/目录/失败类型；本次覆盖普通 NTFS 目录刷新函数入口，未把函数入口等同于所有按钮命中和虚拟视图验证。
  - 你的补充：

- [ ] [#194 小窗模式下，会出现顶部标签页与设置、明暗按钮的UI重叠](https://github.com/jimmgreen/pulse/issues/194)
  - 尚需核实：当前生产自绘布局未见原述标签与顶部设置/主题重叠；离屏测试未替代真实窗口原生非客户区验收。
  - 你的补充：

- [ ] [#147 搜索结果中没有过滤掉不符合要求的文件，以及搜索出来的文件无法空格预览。](https://github.com/jimmgreen/pulse/issues/147)
  - 尚需核实：未实发空格或打开实际预览；搜索框输入路由、焦点、流式结果、内容分页未验收
  - 你的补充：

- [ ] [#9 Previews that resolve through the shell thumbnail cost 40-53 ms on every request when no thumbnail can be produced](https://github.com/jimmgreen/pulse/issues/9)
  - 尚需核实：失败由fixture注入，原XLSX/CAD-PDF样本未取得；未测得原40-53ms；直调host/冷缓存/像素尺寸变化/重启成本仍待测，不据此否认；未复现永久阻塞或所有后续预览失效
  - 你的补充：

## 已有针对实现（静态）（27 条）

本地已有相关处理，但原场景没有完成运行验收，不能直接认定已修复。

- [x] [#374 windows大文件夹无法删除](https://github.com/jimmgreen/pulse/issues/374)
  - 尚需核实：有回收拒绝后再次确认永久删除的实现；未实测超出回收站配额的大目录。
  - 你的补充：

- [ ] [#362 bat 文件以管理员打开不可用](https://github.com/jimmgreen/pulse/issues/362)
  - 尚需核实：显式 runas 不再直接运行注册表 command；管理员完整性级别未实测。
  - 你的补充：

- [ ] [#345 Bug: 微信中打开文件时只打开对应文件夹](https://github.com/jimmgreen/pulse/issues/345)
  - 尚需核实：接管读取并稳定文件选中项；未实测微信“在文件夹中显示”和重复 Explorer 窗口。
  - 本轮补充：已确认当前实验性 Explorer 接管开关关闭，并恢复了设置入口；安装版未替换，微信菜单后的实际接管与文件选择仍未验证。
  - 你的补充：

- [ ] [#324 我按ctrl+F打开了搜索栏，输入内容之后不能点ESC关闭，只能鼠标去点X关闭，效率很低，是否可以增加按ESC关闭搜索栏？](https://github.com/jimmgreen/pulse/issues/324)
  - 尚需核实：FilterEditProc 已处理 VK_ESCAPE 隐藏筛选编辑器；未运行键盘验收。
  - 你的补充：

- [ ] [#296 以管理员身份运行无效](https://github.com/jimmgreen/pulse/issues/296)
  - 尚需核实：runas 有专用动词分支；未实测对应 exe/bat 的 UAC 与完整性级别。
  - 你的补充：

- [ ] [#286 查看图片bug](https://github.com/jimmgreen/pulse/issues/286)
  - 尚需核实：已有默认图片邻图会话；Win10 照片应用的下一张按钮未操作验证。
  - 你的补充：

- [ ] [#276 文件夹内容过大导致删除失败](https://github.com/jimmgreen/pulse/issues/276)
  - 尚需核实：回收站不接受时已有再次确认永久删除分支；附件场景未复现。
  - 你的补充：

- [ ] [#249 Takeover takes over shell-namespace windows it cannot render (phone over MTP)](https://github.com/jimmgreen/pulse/issues/249)
  - 尚需核实：明确过滤非文件系统 namespace；手机窗口本次未实际连接验证。
  - 你的补充：

- [ ] [#222 不能自动定位到文件](https://github.com/jimmgreen/pulse/issues/222)
  - 尚需核实：已有接管文件选中状态和稳定等待；浏览器实际调用链未验证。
  - 你的补充：

- [ ] [#218 [稳定性/资源泄漏] NetworkAgentClient 每秒轮询导致 Timer 与 Semaphore 内核句柄持续泄漏](https://github.com/jimmgreen/pulse/issues/218)
  - 尚需核实：报告中的每请求 Deadline 短命线程已经不在；原泄漏因果存在争议，未做句柄时间序列。
  - 你的补充：

- [ ] [#208 此电脑中，网络磁盘的磁盘名称无法修改](https://github.com/jimmgreen/pulse/issues/208)
  - 尚需核实：本地存在独立 drive_label_rename 模块；网络映射盘显示名重命名未实测。
  - 你的补充：

- [ ] [#196 设置页的滚动条存在异常，只能通过滚轮滚动无法通过左键拉动](https://github.com/jimmgreen/pulse/issues/196)
  - 尚需核实：有 settings scrollbar 命中及拖动计算分支；未视觉操作。
  - 你的补充：

- [ ] [#179 【BUG反馈】在桌面剪切复制文件，没办法粘贴到pulse文件夹里](https://github.com/jimmgreen/pulse/issues/179)
  - 尚需核实：普通 HDROP 和虚拟 Shell 文件均有路径；桌面剪切粘贴尚未实操。
  - 你的补充：

- [ ] [#166 设置的滚动条不能拖动](https://github.com/jimmgreen/pulse/issues/166)
  - 尚需核实：设置滚动条已有拖动分支；远程触摸拖文件另属触摸验证范围。
  - 你的补充：

- [ ] [#150 [bug]关于标签问题](https://github.com/jimmgreen/pulse/issues/150)
  - 尚需核实：tags_loaded 区分空列表与从未保存，已删除默认标签不会因空而自动恢复。
  - 你的补充：

- [ ] [#146 [BUG] 此电脑/磁盘根目录点击顶部「向上一级」按钮，程序崩溃退出（附崩溃转储，1.0.53 必现）](https://github.com/jimmgreen/pulse/issues/146)
  - 尚需核实：空路径 ParentPath 安全返回，未发现报告所述非法路径异常；未真实点击回归。
  - 你的补充：

- [ ] [#126 临时暂存托盘已经影响复制，剪切了！求关掉](https://github.com/jimmgreen/pulse/issues/126)
  - 尚需核实：外部较新剪贴板与托盘批次有序号决策；未重现“单击就进入托盘”现象。
  - 你的补充：

- [ ] [#109 从windows 遠端桌面(Remote desktop) 复制档案，无法在pulse 中贴上](https://github.com/jimmgreen/pulse/issues/109)
  - 尚需核实：虚拟剪贴板 Shell 粘贴有实现；真实远程桌面/Smart Data Link 未验证。
  - 你的补充：

- [ ] [#74 [BUG反馈] 网络位置/SMB共享路径下搜索无结果](https://github.com/jimmgreen/pulse/issues/74)
  - 尚需核实：未覆盖 UNC 可走后台实时文件名遍历；实际 SMB 搜索未验证。
  - 你的补充：

- [ ] [#73 [bug反馈] 在文件夹空白处右键菜单不显示新建菜单](https://github.com/jimmgreen/pulse/issues/73)
  - 尚需核实：native New 顶层初始化已豁免预算；冷缓存菜单原位插入仍需实际验证。
  - 你的补充：

- [ ] [#66 [安全] 索引服务管道对所有已认证用户开放，检索结果不套用 NTFS ACL（多用户机器信息泄露）](https://github.com/jimmgreen/pulse/issues/66)
  - 尚需核实：存在调用者可见性过滤和受限索引目录 ACL；未做多账户服务验收。
  - 你的补充：

- [ ] [#65 [稳定性] 卡住的第三方右键扩展会永久泄漏 pulse_shell 线程（INFINITE join 无法回收）](https://github.com/jimmgreen/pulse/issues/65)
  - 尚需核实：菜单会话可由父进程终止，隔离架构已改变；未注入挂死扩展运行。
  - 你的补充：

- [ ] [#60 [性能] 从压缩包拖拽复制明显慢：无冲突时不必绕临时文件（附实测与取舍）](https://github.com/jimmgreen/pulse/issues/60)
  - 尚需核实：无冲突小文件已有直接写最终名分流，未重测吞吐。
  - 你的补充：

- [ ] [#41 BUG 反馈：右键打不开终端，快速访问编辑徽章无法保存](https://github.com/jimmgreen/pulse/issues/41)
  - 尚需核实：徽章持久化和高级输入框回退有实现；终端失败另见 #165，且该 issue 尚有4条评论未取到。
  - 你的补充：

- [ ] [#40 zip文件无法用pulse打开和空格预览问题](https://github.com/jimmgreen/pulse/issues/40)
  - 尚需核实：本地已有独立压缩包浏览路径；旧 Office 进程残留需对应文档/provider，未验收。
  - 你的补充：

- [ ] [#4 设置里开启了打开方式的选项，但是右键菜单里并没有选择打开方式](https://github.com/jimmgreen/pulse/issues/4)
  - 尚需核实：openas 静态项有兜底文字和独立系统 chooser 路径；未运行菜单验收。
  - 你的补充：

- [ ] [#3 Filter box hides the text you type (the same hosted-editor presentation gap 1.0.13 fixed for rename)](https://github.com/jimmgreen/pulse/issues/3)
  - 尚需核实：共享编辑宿主已重绘输入并支持原生回退；没有本次实际键入画面。
  - 你的补充：

## 行为已确认但缺陷未定（3 条）

已确认实现行为，还需要明确期望，才能判断是否属于 bug。

- [ ] [#377 便携版的配置保存在c盘，疑似bug](https://github.com/jimmgreen/pulse/issues/377)
  - 尚需核实：确认报告者真正预期（当前用户隔离还是随 exe 便携）；本次未更改配置位置，也未将默认存储行为计作 bug。
  - 你的补充：

- [ ] [#220 绿色版没安装索引服务但也会启动pulse.index.exe](https://github.com/jimmgreen/pulse/issues/220)
  - 尚需核实：没安装 Windows 服务不等于禁止用户级索引进程。需用户确认预期及索引总开关的实际状态/版本/命令行；未验证关闭总开关后仍重启的独立故障。
  - 你的补充：

- [ ] [#151 快捷方式显示为*.lnk](https://github.com/jimmgreen/pulse/issues/151)
  - 尚需核实：只确认显示层现象；产品约定未定，不计新增bug。未创建/打开真实Shell快捷方式或修改隐藏扩展名偏好。
  - 你的补充：

