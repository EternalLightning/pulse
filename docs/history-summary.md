# 历史维护摘要

整理日期：2026-10-21。本文归并早期阶段审查、临时方案与重复发布验证，保留已验证结论、维护约束及未解决边界。下列结果均为注明日期的历史记录，本次文档清理没有重跑功能测试；不能据此断言当前代码仍有相同缺陷或当前构建已完成相同验收。

已删除来源可从 Git 历史，或清理前完整快照 `dist/source-snapshot-before-cleanup-20261021.zip` 恢复；逐文件路径、大小与清理前 SHA-256 见 [清理清单](history-cleanup-manifest-2026-10-21.json)。快照还包含未跟踪和被忽略的旧验证附件。来源路径以下用代码标记作为历史定位，不作为仍存在的文件链接。

当前功能说明保留在各主题指南；近期状态以 [Explorer 接管](explorer-takeover-review-2026-10-19.md)、[预览审查](validation/preview-audit-2026-10-21.md)、[动画跟进](validation/animation-followup-2026-10-21.md)、[文本比较与标签](validation/text-compare-tabs-2026-10-21.md)、[标签关闭](validation/tab-close-2026-10-21.md) 及其实际证据为准。

<a id="architecture"></a>
## 架构与资源契约（2026-10-05）

来源：`docs/architecture-interface-review.md`、`docs/architecture-preserving-optimization.md`、`docs/localization-resource-audit.md`、`docs/resource-decoupling-review.md`。

- 保留 UI、索引、文档提取、Shell 与预览的实际隔离责任；阻塞文件系统、COM/provider 调用由原 worker/辅助进程承担。取消、截止时间、身份检查和不可重放删除等语义不同，不能仅为减少重复代码而合并协议或线程池。
- 状态来自枚举、错误码、对象身份与精确路径，资源文本只负责展示。取消统一 `Cancelled`；失败对象使用独立字段，不能从诊断字符串猜测。畸形删除响应拒绝全部部分证据；主程序与 Shell 需配套更新。卷失败位由配套索引程序生产，不能从旧版本显示文案反推。
- 搜索合并改为只读候选引用，只复制最终页；共享菜单 DTO 直接移动；生产 UI 排除网络执行源；排序枚举使用轻量契约头。旧实现对照 16,128 组查询/分页组合无差异，四组定向测试通过。排序仍完整排序，分页仍重取前缀，未测得整程序时延或内存收益。
- 7z 密码诊断仅匹配随附版本的已知整行及当前请求路径；ZIP 列目录成功不证明密码正确。取消、错误对象和显示类别的资源解耦有定向检查；真实系统服务与所有语言/DPI组合不在该轮验收范围。
- 当时 `pulse_file_lock_owner_test` 的 `RmStartSession=29` 在修改前后均出现，根因未查明；不能把它归为此次回归，也不能笼统宣布系统 Restart Manager 不可用。列表/详情扫描后来已有共享服务，不能重新按旧架构快照认定仍重复扫描。

<a id="operations"></a>
## 文件操作、对象身份与恢复（2026-10-07 至 16）

来源：`docs/report.md`、`docs/report-fix-status.md`、`docs/review-fixes-2026-10-09.md`、`docs/pr-1-rename-verification.md`、`docs/validation/archive-retry-recovery-2026-10-15.md`、`docs/validation/1.1.2.0/copy-callback-20261012/review.md`、`docs/validation/1.1.2.0/user-feedback-20261012/rate-results.md`。

- 撤销只接受原操作产生时保存的身份；旧记录缺失身份、替换对象或部分 Move Undo 目标改变时保留未解决历史，不重新采集当前对象后授权删除。跨卷复制式移动通过实际句柄确认源对象后清理；原生回收不得降级永久删除。
- F03 普通 Medium 令牌的原生回收/还原、复制与新建撤销最终 61 PASS / 0 FAIL。工作区 Low 标签程序曾有权限失败；实际 RID 4096 与等字节副本 RID 8192 对照支持环境解释，不要求用户管理员运行。最后一次身份校验与原生 provider 内部按路径操作间仍有竞态，持久历史也不能完全防护 FileID 复用；这是用户保留的原生回收边界。
- 早期失败诊断的三组自身回收项没有持久身份依据，未擅自永久删除。原路径均在 `build/bench_data/opstest-undo-identity-40204-18376375`：`normal-created.txt` 对应 `$RXJI9O9.txt/$IXJI9O9.txt`，`normal-created-folder` 对应 `$R6XU6UP/$I6XU6UP`，`copy-destination/normal-copy-source.txt` 对应 `$RQ9XPWA.txt/$IQ9XPWA.txt`。这只是当时保留状态，清理时未重新查验它们是否仍存在。
- 改名仅接受单个合法文件名；大小写变化实际提交并可撤销。冲突或权限失败不回退到曾静默覆盖的 Shell 改名。原地编辑器使用原生重定向 HDC 后，首次显示、选区、替换输入的桌面像素检查及用户确认通过；保留两张改名前后与两张 toast 图证于 `docs/releases/images/1.0.13/`。
- 解压失败/取消后的重试仍使用原目录，`.pulse-extract-state` 绑定目录身份及规范化源路径，仅清理经报告、句柄与元数据确认的未完成残留；保留无关文件、既有碰撞、被修改/替换/reparse/hardlink输出。真实 7z CRC 与取消恢复 20 PASS，直接提取 19、密码 22、outcome 9 均通过。异常退出缺少最终归属证据时拒绝自动覆盖；不是任意崩溃的自动恢复保证，报告后再开句柄也不等于原子创建溯源。日志读/写上限 32/16 MiB 的极端场景未穷尽。
- `CopyFile2` 只以实际累计字节通知更新节流，开始/轮询不能挤掉有效进度。修复前 10 FAIL / 2 PASS，修复后 12 PASS；真实 F→E 的隔离双 4 GiB 对照及暂停/恢复/取消定向通过。ETA 与显示速率采用同一估计；确定性轨迹最终为 **18 PASS**（早期口头19已更正），不能把回调空白或图表峰值当成实时磁盘吞吐证明。
- 共享目录事实按路径、快照和 generation 接收，失败保留旧行并重枚举；目录大小保留 partial/unavailable/offline 来源。真实慢 UNC、全部重解析类型与失控 provider 仍没有全面验收。合成 HDROP 曾可读，旧 F18 不是普遍剪贴板失效的已验证结论；F21/F28 性能及 F31架构治理需证据后单独决定。

<a id="audit"></a>
## 索引、查询、存储审查最终收口（2026-10-16）

来源：`docs/validation/code-audit-closure-2026-10-16.md` 及当日 `code-audit-*-closure-2026-10-16.md`。早期 `code-audit-outstanding-2026-10-16.md`、Core/App/UI覆盖表和逐轮报告已被后续收口接替；其中 U01–U12、R01–R08 不能继续当作未修复清单。

原清单 Core 157项、App原28项、UI原18项完成全文阅读，Ops/FS/Shell建立66文件台账；只描述当时清单，不宣称当前全仓覆盖或无缺陷。最终分组断言 Engine159、query85、network60、content88、host/config54、storage/diagnostics63、App44+39、UI72、Ops26，共 **690 PASS / 0 FAIL**。含准备/清理断言，不等于690独立产品场景，共享selector不重复相加。逐项最终记录如下。

| ID | 处理结果 | 实际验证与依据 |
| --- | --- | --- |
| U01 | 持久化全局 Add ID、跨卷按身份重放；旧/坏日志保留并要求重建 | Delta 25/0；含再次追加/启动、FRN/checkpoint、伪造范围拒绝、canonical/letter V1、隔离重建发布与重启；原实现有效失败对照 10/3 |
| U02 | mmap 及内存重建验证父链、卷布局；离线保留父先子后 | mapped 24/0、parent-cycle 14/0，structure 覆盖移动后离线 FRN 保留 |
| U03 | 父节点变化后禁用旧 DFS scope 快捷路径 | structure 71/0 的移动/改名/新增后代与父链 oracle |
| U04 | posting 与 patch 候选去重 | 同一 structure selector 的稀疏/密集、拼音、total/分页 |
| U05 | 保留完整深路径，拒绝未解析/循环 FRN | 同一 structure selector 的 48/64/256/300 层、父先/子先及排除 |
| U06 | 排除快照在同一 mutex 下发布 | exclusions-concurrency 5/0，10 次实际隔离重建及并发读者 |
| U07 | 否定整个 file/folder 名称及类型谓词 | query-boundaries 53/0、folder-kind-negation 32/0、content filename 17/0；Engine 实际集合 20/0 |
| U08 | 统一规范化完整路径谓词及绝对路径组件边界 | 同组 query/content/Engine 检查，包含 skip/skip2、多绝对条件及相对路径 |
| U09 | 严格 JSON/Unicode，区分六类加载结果；错误后禁止覆盖原文件 | network-config 60/0 |
| U10 | 部分枚举失败保留命中并报告不完整 | transient-errors 9/0、task-enumeration 11/0 |
| U11 | 16 查询连接加控制实例，饱和拒绝而保留已有查询 | agent-capacity 11/0；取消后恢复接入、其它订阅存活 |
| U12 | 重建命令传播 SCM 失败及非零退出码 | host service 5/0；上层 bool/失败反馈静态核对，未控制真实服务 |
| R01 | roots 非空时覆盖兼容 root | 生产 wire、缓存、task 双根/单根 content-contract 6/0 |
| R02 | 暂时属性失败阻止错误 prune | shared-retention 12/0；恢复更新和真实删除对照 |
| R03 | 原 UI 已消费错误；统一不完整根状态 | root-errors 6/0；不将原行为误写成“UI 隐藏错误” |
| R04 | 迁移全程持目录/文件租约，按拥有句柄回滚；不支持元数据拒绝迁移 | identity 6/0，metadata 17/0，默认专用迁移 Medium 副本 30/0；原错误回滚对照 4/2 |
| R05 | 区分正常 EOF 与枚举/属性错误，失败不写成功 manifest | enumeration-errors 10/0；原实现 3/6，含部分副本已生成后失败 |
| R06 | 批量重命名预览单 worker、最新代际、取消及关闭不阻塞 | batch_rename_preview 9/0 |
| R07 | watcher 会话邮箱和 stop HANDLE 真实所有权 | shell_registry_watch 8/0；超时后 worker 完成与会话更替 |
| R08 | 数值溢出/非法日期拒绝；机器 JSON 与 SQLite/sidecar 失败不退回默认扫描 | query 53/0；host machine 25/0、content 24/0；content-config-errors 6/0；Engine 调用者严格失败处理 |

App额外修复选择意图被导航清空、分屏命中/绘制间距、色块动作ID、模态菜单向量指针及空tags复活。UI修复popup工作区拟合、滚动子菜单锚点、窄详情宽度、预览关闭后滚动高度、跨DPI字体、超过96个文本hit范围，以及冷关联COM从绘制移到现有worker；高对比配色有局部修复，未做该轮完整视觉批准。Ops修复目录Skip后的元数据收尾、snprintf所需长度误用及回收站v1缺终止符。

内容容量初版夹具仅改LOCALAPPDATA，初始化超时前可能读取真实sidecar；最终改为显式隔离DB、agent、mutex与pipe。无运行前hash，不能宣称初版全程未读写用户配置。迁移默认测试等字节Medium副本30/0支持Low权限解释，早先无输出的具体异常点并未反推查明。

真实多卷USN/卸载、断电、磁盘损坏、SMB/RaiDrive/云盘阻塞、真实SCM重建及完整provider验收仍不由上述隔离断言证明。用户级存储迁移不支持的元数据采用拒绝策略，不能声称完整复制ADS/EFS/稀疏；安全属性/EA部分拒绝分支只有源码依据。诊断导出没有跨文件快照或原子清理保证。

<a id="recovery"></a>
## 分片恢复与缓存加载（2026-10-16）

来源：`docs/validation/code-audit-shard-recovery-2026-10-16.md`、`docs/validation/code-audit-shard-io-rebuild-2026-10-16.md`、`docs/validation/code-audit-cache-loading-2026-10-16.md`。

- `Missing/Invalid/IoError/Valid/Unsupported` 区分不存在、确证损坏、不可读与未来版本。I/O/Unsupported停止恢复或发布，不能误回退、扫描孤立槽或消耗候选；确证双槽坏/缺失才允许健康重建。机器缓存读取直接遵守分片解析和CRC，不再绕过manifest映射A槽。四个selector最终44+15+13+12，共84 PASS，保留旧失败对照。
- 仍未验收：多次打开/检查/哈希/移动之间的替换竞争、并发发布串行化、固定 `manifest.json.tmp`、父路径替换、未知未来版本迁移、同槽异常布局自动重建。previous没有历史hash基准，重算只证明本次完整读取；manifest字段检查不等于通用完整JSON parser。
- `WriteVolumeShards` 当时未上抛发布失败，可能遗留候选，但未发布世代不被当作已提交分片。非机器 `TryLoadCache` 映射失败后仍按旧行为删除缓存，未细分映射失败原因，需单独复现后判断。上述为历史待核实边界，不是当前已验证bug。

<a id="candidates"></a>
## 保留的未确认候选与验收缺口（2026-10-16 快照）

来源：当日 `code-audit-small-document`、`code-audit-small-ipc`、`code-audit-small-index-helpers`、`code-audit-change-tracking-full`、`code-audit-document-full`、`code-audit-small-storage-probes`、`code-audit-small-timing`、`code-audit-ui-closure` 报告。以下均不能因为清理而升级为当前缺陷或已通过项；后续先复核当前代码与实际触发。

| 候选/边界 | 历史依据与下一步 |
| --- | --- |
| PDF补充平面字符 | `pdfium_text.cpp` 调用GetText的本地头契约为UCS-2，可能丢U+20000；尚未用随仓DLL及明确ToUnicode样例证明。先比较BMP、`A+U+20000+B`、纯补充平面字符的GetUnicode/GetText与实际内容查询，不直接修代码。 |
| Shell/诊断分配异常 | Shell查询前置分配不全在HRESULT异常转换范围内；部分诊断裸句柄异常退出无独立RAII。无普通输入动态复现，保留为加固候选。 |
| 权限和迁移路径竞争 | `index_caller_visibility.h`逐祖先授权后重开完整路径、服务迁移预检到按路径执行之间仍非全程句柄锚定；祖先动态替换未测，不据此定为可利用漏洞。源保护只允许已识别索引artifact，普通资料ACL不可递归改写；权限回调18/0不是实际SCM/DACL验收。 |
| deadline范围 | 授权2秒、服务启动等待次数不保证阻塞系统调用的端到端硬上限；feed跳过status循环缺总deadline尚无合法host长期触发复现。Shell helper回收尾部可能另等1秒。 |
| 排除规则与服务状态 | 特殊目录除node_modules外曾大小写敏感，需产品规则确认；正常索引迁移曾启动原来停止的服务，尚无当时“保留停止状态”规则，未定缺陷。 |
| 文档保真与日志 | OOXML关系顺序、MC分支、图形/公式文字、显示格式未由代表样例证明完整保真；diagnostic短写可能遗留尾部不完整记录，属于best effort，进程内存样本不等于组件独占内存。 |
| 可访问性与provider | 实际自定义高对比、文字缩放/读屏、跨屏100%→200%输入字体、极小popup按钮、长文件高亮clipping、慢关联COM与30秒负缓存Loading→fallback均待针对性实测。原生EDIT/IME、系统选择器、tooltip及第三方预览窗口保留平台职责，不机械替换。 |

<a id="performance"></a>
## 调度、缓存与体积优化（2026-09-22 至 10-13）

来源：`docs/lumatext-size-optimization.md`、`docs/validation/ui-scheduling-2026-10-13.md`、`docs/validation/pane-animation-2026-10-13.md`、`docs/validation/search-ui-optimization-2026-10-13.md`、`docs/validation/resource-waste-2026-10-13.md`、`docs/validation/runtime-idle-work-2026-10-13.md`、`docs/validation/structure-cleanup-2026-10-13.md`、`docs/validation/dead-code-cleanup-2026-10-14.md`。

- 动画在Render构建视图前按steady_clock推进，复用WM_PAINT/Present续帧；维护定时器保留状态/焦点交接职责。搜索/排序复用模型统计，稳定选择的根目录判断命中修订缓存，showFps关闭时停止采样；不以降低帧率或拉长有效性检查周期换开销。
- 暂存区用路径修订号避免不变输入反复收集路径；元数据耗尽后休眠并在新增/失败重试时唤醒；worker锁内移除失效订阅的旧快照，但不强制释放运行任务正在使用的数据。10,000次输入只收集一次为次数检查，相关四组162 PASS，不是帧率/CPU基准。
- GPU图标仅按CPU淘汰代际移除失效index，保持有效位图。可见缩略图像素、设备恢复资料和有效缓存不当垃圾清空。20,000项稳定选择的单处判断局部基准不推广为整程序收益；未量化整机CPU/GPU/功耗与显存净变化。
- LumaText Release保留renderer `/O2`，依赖 `/Os`、IPO、`/Gw`；当时DLL从1,814,528降到1,648,128字节（9.17%），26导出不变，像素hash一致，4项测试及断言启用的ABI消费者通过。样例CPU均值约942k→971k cycles/frame，有小幅成本，不能声称普遍更快。重建方法与CRT模块内分配/释放约束继续见 [静态运行库指南](static-runtime-packaging.md)。

<a id="releases"></a>
## 发布、权限告警与多窗口（2026-10-11 至 20）

来源：`docs/portable_test_20261011.md`、`docs/validation/1.1.1.0/`、`docs/validation/1.1.2.0/`、`docs/validation/release-deploy-2026-10-16/`、`docs/validation/1.1.3.0/`、`docs/validation/1.1.3.2/review.md`。旧版本发布说明继续保留在 [releases](releases/)。

- 配套部署五个EXE及DLL/7zip/许可证，核对版本、OriginalFilename、ZIP路径及每项hash、运行库依赖。更新前保留完整可核验回滚，替换实际变更项并保存偏好、服务与网络agent状态。WM_CLOSE可能仅隐藏/驻留，不能据此认定主程序退出。
- 1.1.1.0预览ACK改为有效像素视图；1.1.2.0将管理员文件操作放入既有Shell，保留标准UAC、确认/撤销/身份检查。10-12原生UAC隔离复制6 PASS只证明该fixture，未完成真实UI点击全链路。旧检测与当时零检出都只适用于相应字节和本机规则；用户撤回外部提交后停止，上传曾失败，无成功受理回执或厂商误报结论。
- 1.1.3.0恢复日志状态改为配置时才创建，使Shell worker不初始化应用日志容器；对照构建把当时告警范围缩小到该公共状态及生成代码，不代表掌握杀毒私有判定，也不是未来永不告警承诺。
- 标签合回曾因关闭任一窗口停止共享Shell、回调只归首窗口而无法打开文件。修复为每窗口使用权、每请求回调归属、最后使用者才停止；打开线程上限由2改64，为最多32窗口及退休线程留界。31项Shell、3项五窗口worker和真实窗口50 PASS/0 failures（8次实际EXE写标记）通过。WndProc转移夹具不等于物理鼠标拖放；PNG默认查看器、Outlook/RDP虚拟文件、跨屏DPI未专项验收。系统“打开文件-安全警告”等待确认不能判成启动成功或某个防护产品故障。
- 1.1.3.2包含180ms首帧淡入、动画关闭/高对比恢复不透明、局部亚克力采样和只读VM命中。历史部署记录曾服务Stopped无权恢复；**后来1.1.3.3记录已恢复Running/Auto**，不要把旧状态当当前事实。未测GPU帧时间和整机资源净变化。
- 1.1.2.0旧发布包与部署证据被后续版本接替；保留发布说明中的确切历史包hash，不能拿旧扫描或旧测试结果背书新载荷。最近完整终端修复、40文件hash和回滚/服务证据继续保留于 [1.1.3.3发布记录](validation/1.1.3.3/terminal-release-20261020/review.md)。其26 PASS覆盖菜单到启动队列；真实Windows Terminal窗口中的当前目录仍未实际点验。

## 本次保留与清理原则

保留现行功能/存储/兼容/打包指南、旧版本简明发布说明、README使用的产品图、少量1.0.13前后图，以及2026-10-19至21的当前审查和交互证据。两个HTML交互方案和视频封面/上传说明仍有参考或发布用途，未仅凭时间删除。最新1.1.3.3发布目录完整保留，当前release-cleanup工作及tab-close/animation/textcompare证据不在清理范围。

删除已被本文接替的阶段报告、旧验证日志/重复截图/一次性部署脚本与fixture；仅逐文件删除已解析并确认位于 `E:\Projects\pulse\docs` 内、无reparse祖先的普通文件，再非递归移除空目录。未更改生产源码、构建脚本、用户配置或安装目录，没有删除回滚包。
