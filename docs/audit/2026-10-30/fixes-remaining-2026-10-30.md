# 剩余审查项修复与最终验证（2026-10-30）

对应[原审查](../../code-audit-2026-10-30.md)和[覆盖索引](README.md)。原审查共有 75 个唯一编号：73 个已确认项，另 UI07/UI08 两项候选。前两批已处置 12 项；本批处理剩余 61 个已确认项，并通过受控复现处理两项候选。下表区分源码处置、实际运行和未测边界，不把编译、SKIP 或预期故障记作功能通过。

起点仍为 `f416555c`，版本 `1.2.2.0`。保留前两批修复、侧栏项目动画和其他会话的许可证/构建改动。本轮未提交、安装、打包或发布。文件操作继续保留对象身份、恢复、失败、确认及 Undo 契约；提权协议 v2 需要同步交付对应主程序和 helper。

## 逐项处置

“前批”表示沿用[第一批](fixes-2026-10-30.md)或[第二批](fixes-batch2-2026-10-30.md)的记录，本轮没有机械重跑所有历史回归。“候选复现”只表示本轮有受控反例，不表示真实远程 provider 或所有输入法均已验收。

| 编号 | 原级别 | 批次 | 修复内容 | 实际验证及边界 |
| --- | --- | --- | --- | --- |
| AC01 | P2 | 前批 | 稳定命中身份与显示代次校验，避免旧行号选中其他文件 | 第一批记录 |
| AC02 | P2 | 前批 | 目录通知后台补齐快捷方式事实，与完整枚举一致 | 第二批记录 |
| AC03 | P1 | 前批 | 菜单回调重新定位原 Tab，不跨消息循环保留裸引用 | 第一批记录 |
| AC04 | P1 | 前批 | 徽章编辑回调通过稳定条目身份重新定位 | 第一批记录 |
| AC05 | P2 | 本批 | 卷名与容量在 HomeCatalog 后台收集，代次阻止旧设备事实发布 | 23 项独立回归及生产烟测 |
| AC06 | P2 | 前批 | 诊断写出长度限制在格式化缓冲区有效范围 | 第一批记录 |
| IP01 | P2 | 本批 | 属性切换后台串行执行；邮箱校验窗格、Tab、代次及详情路径 | 真实属性/权限回归；生产对象生命周期回归 |
| IP02 | P2 | 本批 | Esc 清除四类侧栏排序手势及 capture，松键不再提交 | 生产 HandleKeyDown/HandleLButtonUp 生命周期回归 |
| IP03 | P2 | 本批 | 主窗口转发 Alt+Up/Left/Right 的系统按键 | 实际 WndProcImpl 系统按键路由回归 |
| IP04 | P2 | 本批 | 延迟点击保存窗格、Tab 身份和代次，布局重绑统一取消 | 生产选择模型跨 Tab/代次正反向回归 |
| AI01 | P2 | 前批 | 设备请求代次及超时退休结果隔离 | 第二批记录 |
| AI02 | P2 | 本批 | 清空标签只将 ADS 已不存在或实际删除成功视为成功 | 隔离 NTFS ADS 共享冲突及重试 |
| AI03 | P2 | 本批 | 执行目标、图标和读取失败独立建模；仅明确缺失目标可清理 | 私有 HKCU 根及 DACL；集成组 57 项 |
| AI04 | P2 | 本批 | 不确定树移动进入 RecoveryRequired；两端兼容内容安全恢复 | 私有 HKCU 故障、冲突、备份与重复恢复；57 项 |
| AI05 | P2 | 前批 | 同目录云操作结果合并刷新，避免重复全选捕获 | 第二批记录 |
| OF01 | P2 | 本批 | 设备 watchdog 在实际执行时启动，迁移保留预算；退休 lane 上限四个 | 真实线程/timer；操作组 66 项 |
| OF02 | P2 | 本批 | 设备枚举保留 HRESULT/取消；失败不覆盖最后完整快照 | 受控 COM 枚举；操作组 66 项 |
| OF03 | P2 | 本批 | 统一 Cloud tag 分类，保留 FileID、祖先固定和目录 lease | 真实句柄/身份、受控 Cloud tag；67 项 |
| OF04 | P2 | 本批 | UNC/扩展 UNC 共享根统一分解；空组件明确失败 | 四类根及子项准入；操作组 66 项 |
| OF05 | P2 | 本批 | 递归云枚举错误累计 failed，保存首个错误和路径 | FindFirst/Next 故障、空目录、取消；66 项 |
| OF06 | P2 | 本批 | 此电脑、回收列表、占用、清空计划共用 remote-like 策略 | 四个真实入口的受控 API 回归；8 项 |
| OF07 | P3 | 本批 | 提权协议 v2 传完整冲突 rows/resolutions，共用决策合法性检查 | 生产 codec 批量、截断、版本、链接确认回归 |
| OF08 | P2 | 本批 | 暂存创建/清理固定对象身份，按句柄处理叶节点和 reparse | 真实 junction、替换尝试、失败恢复、只读源；66 项 |
| IX01 | P2 | 本批 | 初扫/即时订阅/别名共用多根请求范围谓词，处理祖先事件 | 双根 watcher/shared 生命周期；三类目录事件回归 |
| IX02 | P2 | 本批 | 流式成员仅更新受影响身份，排序走磁盘索引；源批量投递 | 1k/10k/100k 精确结果、筛选、排序、选择及线性写入 |
| IX03 | P2 | 前批 | feed/change client 复用带服务身份验证的连接入口 | 第一批记录 |
| IX04 | P2 | 本批 | 仅对重叠网络根汇总不完整/扫描/离线/错误状态 | 真实 shard + 受控枚举/工作线程；21 项 |
| IX05 | P2 | 本批 | 启动每卷最多四个 256KiB USN 包，逐包验证/应用/提交 checkpoint | 真实 DeltaLog 重启重放、有限目标、错误/取消；15 项 |
| IX06 | P2 | 本批 | MFT 后备按 FRN 读取真实属性；失败不发布零属性新快照 | 真实 FileID 属性及实际查询/汇总；13 项 |
| PC01 | P2 | 前批 | TypeName 属性响应接受当前协议有效载荷 | 第一批记录 |
| PC02 | P2 | 本批 | 搜索表单保留大小/日期及不可单字段表达的原约束片段 | 新模型 20 项；旧模型 14 个反例；既有搜索 27 项 |
| PC03 | P2 | 本批 | 仅大小写变化可提交；同名目标继续核验真实对象身份 | 隔离 Win32 改名及现有预览 9 项；敏感目录 SKIP |
| PC04 | P2 | 前批 | Shell Request 全分支 RAII 所有权 | 第一批记录 |
| PC05 | P2 | 本批 | 完整 JPEG 读取 EXIF 1–8，与嵌入图方向契约一致 | WIC 37 项；旧实现 28 个反例；嵌入图 42 项 |
| PC06 | P3 | 本批 | 移除不可达旧 IFileOperation 尾段和专用 sink，保留真实删除/回收保护 | 所有权 12 项；四种入口编译；真实删除未运行 |
| PC07 | P3 | 本批 | 文本占位判断直接复用 fs；PINNED 意图不等于已驻留 | OFFLINE/RECALL/PINNED 组合及 cloud policy 回归 |
| UI01 | P2 | 本批 | 预览只在实际 resize 时重置滚动，普通绘制保留排队位移 | 真实 HWND/Render 的滚动、反向、resize、限幅 |
| UI02 | P2 | 本批 | 共用自绘与 EDIT 焦点顺序，补 Tab/Shift+Tab/Space/Enter 和焦点态 | 生产 Luma HWND 定向 79 项；实际提交/回调及截图 |
| UI03 | P2 | 本批 | 对话框最小尺寸随 DPI/冲突详情展开状态更新 | 96/144 DPI 消息、真实展开/折叠及截图 |
| UI04 | P2 | 本批 | 高对比度选中/hover 前景配对系统 HighlightText，禁用态保留系统色 | 真实菜单受控 HC 四态像素；列表路径源码核对 |
| UI05 | P2 | 本批 | 菜单每次读取 owner DPI，已有 filter EDIT 同步更新字体 | 同一真实 EDIT 字体 14→21→14 px |
| UI06 | P3 | 本批 | 颜色弹窗通过 WM_TIMER 动画，持续分派消息并遵循动画/HC 偏好 | 自有真实 popup Escape 分派 0 ms |
| UI07 | P2 | 候选复现 | 有界 identity I/O 自有任务，stop/500ms miss，最多四个迟到探测 | 真实文件身份及阻塞/取消/上限；19 项联合回归 |
| UI08 | P2 | 候选复现 | IME 预备删选区封装事务；取消恢复，提交保留原生 undo | 真实 EDIT 受控 IME 消息；19 项联合回归 |
| TA01 | P2 | 本批 | 菜单按 command/真实命中派发断言，跳过真正禁用 Undo | 源码 /Zs；可用 audit-test-contracts filter，未运行 |
| TA02 | P2 | 本批 | 设置测试复用实际 tile 布局/painter，移除已删除下拉操作 | 源码 /Zs；真实设置窗口流程未运行 |
| TA03 | P2 | 本批 | 清空查询核对恢复 origin、取消旧代次及晚到结果隔离 | 源码 /Zs；可选自测 filter 未运行 |
| TA04 | P2 | 本批 | 先证明 Share 父组可见，再验证单项隐藏与持久化 | 源码 /Zs；可选自测 filter 未运行 |
| TA05 | P2 | 本批 | 自测各自新建 GUID 夹具及 CREATE_NEW，清理仅作用于自有目录 | 源码 /Zs；相应自测未运行 |
| TA06 | P2 | 本批 | 外部文本通过固定格式 LogText 输出，保留字面百分号 | 源码 /Zs；可选自测探针未运行 |
| TA07 | P2 | 本批 | 流式像素断言使用当前映射 BGRA，不再解码磁盘旧图 | 实际 motion 双模式验证见收尾表 |
| TA08 | P2 | 本批 | 窗口自动化核对 PID/UI 线程/owner/class/visible，操作前再验 | 独立身份回归 9 项；完整删除 UI 未运行 |
| TB01 | P1 | 前批 | 内容/实时测试隔离选中数据库、默认配置及 locator sentinel | 第一批记录；本轮端到端 sentinel 复核 |
| TB02 | P2 | 本批 | 移动盘策略按真实系统卷/夹具卷事实，移除 C:/E: 假设 | 只读盘策略和自有夹具 44 项 |
| TB03 | P2 | 本批 | 保存搜索测试使用自有唯一目录，数量前提后再索引 | 两进程并发，各 10 项 |
| TB04 | P2 | 本批 | 析构计时先证明当前请求确已进入受控阻塞 | 带正式 testing define 的 /Zs；GPU 用例未运行 |
| TB05 | P3 | 本批 | exFAT 证据写实际 UTC/HEAD/新目录，保留历史证据 | /Zs；未访问真实 USB |
| TB06 | P3 | 本批 | 播放截图 CREATE_NEW，核对每次写出、flush、close 和长度 | /Zs；未运行真实播放/GPU截图 |
| TCC01 | P2 | 本批 | 取消回归在自有 kill-on-close Job 内运行，父 deadline 有界 | 有意永久 stall 探针有界失败；目录子进程实际运行 |
| TCC02 | P2 | 本批 | 旧会话拒绝精确核验失败 HRESULT，不能接受成功回复 | 真实 context 进程完整 reuse 31 项及定向 8 项 |
| TCC03 | P2 | 本批 | contenttest 使用唯一自有夹具，失败保留 | /Zs；完整 fixture-only 双进程未运行 |
| TCC04 | P3 | 本批 | RAII 成功清理、失败保留，所有返回统一结算 | 原 guard 故障探针；本轮失败目录实际保留 |
| TZ01 | P2 | 本批 | 缩略图测试精确核对响应类型、文本、属性和像素载荷 | 原断言片段受控反例 11 项；完整传输未运行 |
| TZ02 | P3 | 本批 | 动画回归限制两秒模拟帧，超限报告失败 | 正常 46 项；故障探针有界 4 个预期失败 |
| TO01 | P2 | 本批 | 剪贴板段隔离到私有 station 子进程，失败不回退用户剪贴板 | 本环境明确 SKIP77；无交互式剪贴板写入 |
| TO02 | P2 | 本批 | 删除测试不全局确认用户窗口，仅操作自建 helper 窗口 | /Zs；真实 UNC 删除未运行 |
| TO03 | P2 | 本批 | UNC 确认按 task/token/path/disposition 及终态核验 | /Zs；自有共享真实删除未运行 |
| TO04 | P3 | 本批 | 只断言自己成功创建的映射，取消 Z: 依赖 | /Zs；未创建实际 SMB 映射 |
| TO05 | P3 | 本批 | 提交拒绝或 timeout 立即失败/取消/中断，禁止使用旧任务终态 | 当前 helper 的提交拒绝、超时、错误终态故障探针 |
| TO06 | P3 | 本批 | 冲突三行数量前提成立后再 front/back/pop_back 变异 | /Zs；真实批量冲突夹具未运行 |
| TGM01 | P2 | 本批 | 基准只在完整匹配响应被解码后计成功 | 生产读取/解码函数独立 20 项 |
| TGM02 | P2 | 本批 | MFT 页头续读游标独立解析，复用生产 USN 校验 | 解析 8 项；未扫描原始用户卷 |
| TGM03 | P2 | 本批 | Shell 基准从创建前计时，失败状态/耗时全部初始化 | 创建失败独立 1 项 |
| TGM04 | P1 | 前批 | 全局搜索窗口测试关闭持久化并使用隔离 profile | 第一批记录 |
| TGM05 | P3 | 本批 | BMP 保存核对创建/写出/flush/close/长度并返回真实路径 | 独立生产目标 8 项；完整编辑器 GUI 未运行 |

## 跨模块修复与测量

普通索引压缩替换的是按行序号分页的快照，不代表按路径记录的事件断流。将 snapshot token 与事件 epoch 分离，普通映射替换保留有界事件历史；真实 FullRebuild/MFT 恢复及 GapFeed 仍使旧事件订阅失效。共享内容 reconcile 为每个根保存独立快照 token，不再与连续事件 epoch 混用。这处理了前批删除后错误 `feed_gap/1022`、订阅结束并残留旧结果的问题。

feed 定向测试核对普通映射替换、快照游标失效、事件连续性、有界保留、真实 gap、重新订阅和内存诊断。首次失败来自沿用旧“一条事件”内存预期，随后新加的“gap 必须清空历史”假设也被实际契约否定；最终保留准确的 100,000 条有界历史及新 epoch 断言，失败日志未删除。

首次百次运行中，Instant 的 100 次删除全部收敛，但 P95 为 578 ms，未达 300 ms；Legacy 在第 13 次创建时超出五秒。trace 指向共同上游：十二次删除各触发一次普通索引压缩，最慢的一次整体耗时 5,009 ms，后续通知在压缩完成后才被处理。小索引的一条 tombstone 即超过删除比例门槛，而该门槛绕过了已有十分钟合并间隔。现在删除比例遵守原间隔，100,000 条结构变化及 64 MiB delta 硬预算仍可立即触发；正常 delta 落盘、提交/回滚、显式强制合并和事件连续性保持。新回归在旧实现为 22 PASS/3 FAIL，修后为 25 PASS/0 FAIL。证据只确定整个同步合并耗时，未确定其中具体 I/O 阶段，也不宣称全程持有同一把锁。

Instant 另去除客户端重复等待：服务端对空页已经执行最多 100 ms long-poll，客户端页尾等待由 150 ms 缩至 10 ms；消费者原有 150 ms 编辑器事件合并保持。最终百次测量在所有构建与 GPU 测试停止后单独执行，结果见下表，不用十次诊断替代原百次验收阈值。

流式结果不再每个命中重建完整 visible 表。1k/10k/100k 实测成员写入分别为 999/9,999/99,999，首条立即可读，筛选、排序、修改、删除、稳定身份、选择求和和删除后 Append 均有断言。十万条无远页缓存的先前对照约 1.75 秒；正式测试中途缓存远页后为 17.27 秒，最慢一批 53.23 ms。远页 OFFSET 仍会读取放大，不能把线性成员写入宣传为所有滚动位置均线性耗时。

独立 watcher 的祖先改名测试曾在初扫首条命中后返回 Win32 5。直接 Win32 探针证明：即使 FILE_SHARE_DELETE，活跃后代句柄也可能阻止祖先目录改名。用例现在等待真实初扫完成屏障，再执行改名；原改名成功和旧结果移除断言均保留。三种确定性目录事件回归另行覆盖局部替换、请求祖先和配置根祖先，不声称这些模拟事件覆盖真实扫描尚持有句柄时成功改名。

## 实际收尾验证

所有运行副本均先与构建/私有探针源文件核对 SHA256；可执行文件从私有 Medium 目录运行，未调整生产文件 ACL。profile、数据库、注册表根和文件夹具均隔离；失败材料保留。

| 检查 | 实际结果 |
| --- | --- |
| 生产 `pulse` 及五个打包 helper 的 Release 构建 | 成功；`PULSE_WITH_SELFTEST=OFF`；无新增编译警告 |
| 定向 CMake 测试目标及正式 Luma HWND 目标 | 成功；最终日志见下方证据目录 |
| Task 流式 / store realtime / 十万条成员写入 | 全部断言通过 |
| 目录模型链接及目录合同 | 缺失 cloud source 依赖已补；定向断言通过 |
| 双根 Instant watcher / shared 生命周期 | 最终各自退出 0；创建、修改、改名、删除、暂停、取消和旧代次隔离均通过 |
| 三类目录事件及父 Job deadline | 退出 0；子进程在 deadline 内完成 |
| 使用当前生产对象的 IP01–IP04 生命周期 | 退出 0；Esc/Alt/跨 Tab/晚到属性回调通过 |
| 当前生产 LumaText 配置的真实 HWND | 79 PASS，0 FAIL；96/144 DPI、焦点/激活/提交、最小尺寸、HC 菜单像素通过 |
| 最新 identity / IME fixture | 19 PASS，0 FAIL；真实文件身份及受控 I/O/IME 边界 |
| BMP artifact 正式目标 | 8 PASS，0 FAIL；实际像素、路径、失败及清理 |
| 真实生产窗口烟测 | 亮色非空目录、暗色空目录均正常加载/渲染/退出 0；1280×800 PNG 已开图核验 |
| 默认 Instant 百次真实创建/删除 | 每次均收敛；创建 P50/P95/P99 为 188/219/234 ms，删除为 172/187/188 ms；原 1000/300 ms 的 P95 阈值通过 |
| LegacyWriter 百次真实创建/删除 | 每次均收敛；创建 P50/P95/P99 为 422/500/516 ms，删除为 156/219/250 ms；相同阈值通过 |
| 最终生产新鲜度和运行 payload | 收尾 `pulse` 构建无待构建项；六 EXE/两 DLL 与最后烟测运行副本 SHA256 一致 |
| TA07 实际 motion 双模式 | 流式/文件模式各 11 PASS、0 FAIL；各自四次当前帧像素检查通过，执行副本 SHA256 一致 |

附加分包验证包括：卷/属性 23 项、私有注册表/ADS 57 项、ops/fs 66 项、Cloud guard 67 项、remote-like 8 项和最终 codec；USN 15、MFT 属性 13、网络覆盖 21 项；模型 20、既有搜索 27、JPEG 37、嵌入图 42、所有权 12、改名预览 9 项；测试工具独立 46 项；移动盘策略 44、动画 46、保存搜索并发各 10、context 完整 reuse 31 及定向 stale 8 项。分包检查与主线程重跑有交集，不将这些数字相加冒充独立功能覆盖。

故障探针中有意永久等待、未停止动画、失败提交、坏协议以及旧实现反例均按预期失败记录，未当作正常产品 PASS。私有 clipboard station 为 SKIP77；case-sensitive 目录启用为 Win32 5/SKIP。历史 context 重定向运行和网络初次运行的失败日志均保留，最终隔离运行成功不用于臆测最初失败的具体系统根因。

## 保留的设计与未测边界

Native/Win32/UNC/MTP/提权后端、COM 进程隔离、对象身份与 Undo 的能力不同，保留各自契约；没有仅凭外形重复就合并安全边界。Explorer 接管、默认管理器恢复、Pulse 菜单偏好与系统注册表菜单管理作用域不同，不增加另一套重复设置。没有证实的 breadcrumb/provider 尺寸/归档生命周期等候选观察未伪报为已复现缺陷，也未借本轮做大范围重构。

未执行完整历史回归、真实 MTP 插拔/写入、SMB 传输或断网、Cloud provider 水合复制、真实远程型挂载阻塞、全卷原始 USN/MFT 故障、真实 UAC GUI、物理跨屏、Microsoft Pinyin 候选窗和完整屏幕阅读器验收。高级搜索/IME/云 tag 的受控检查不能替代这些边界。

TA01–TA06 的可选自测及实际设置窗口、完整删除/回收/UNC 确认、播放截图/材质 GPU、完整 thumbnail transport 与部分较大测试只做本表注明的定向静态检查或局部探针；没有将跳过用例写为通过。严格 MFT fallback 遇不可读取属性会保留旧快照并重试，可能使首次后备构建暂时不可用；没有将未知大小/时间伪造为零。

## 证据与复现

本轮完整证据在仓库忽略目录 `bench_data/audit-fixes-20261030-remaining/`：`production-build.log`、`validation-build.log`、`final-build.log`、`final-fixture-build.log`、`final-production-build.log`、`production-freshness.log`、`production-smoke-final.log`，以及 `app-core.md`、`integrations.md`、`ops-fs.md`、`index-engine.md`、`common.md`、`ui.md`、`tests-am.md`、`tests-nz.md`、`ancestor-rename.md`、`merge-latency.md`、`ta07-motion-runtime.md`。每个分包报告明确源码、故障注入、实际命令、失败与未测边界；主线程最终记录以本文件和各 `*.out.log/*.err.log` 为准。

正式可重复的可选目标已加入 CMake，均不进入默认生产构建：`pulse_content_stream_scaling_test`、`pulse_app_core_remaining_test`、`pulse_app_integrations_recovery_test`、`pulse_audit_common_remaining_test`、`pulse_jpeg_orientation_test`、`pulse_audit_ui_identity_ime_test`、`pulse_audit_ui_remaining_test`、`pulse_delete_confirmation_owner_test`、`pulse_bitmap_artifact_test`。JPEG 真实夹具用 `scripts/generate_jpeg_orientation_fixture.py <不存在的新目录>` 生成，需要 Pillow。旧内容/索引测试使用现有选择器，不默认跑整套。

主线程日志分组为 `final-checks/`（feed、目录）、`e2e-checks/`（生产对象输入及十次诊断）、`release-fixtures/`（最新 watcher/shared/真实 Luma/IME/BMP）、`realtime-full/`（首次百次失败测量）和 `realtime-final/`（最终 25 项维护策略、20 项 feed 及两种模式各百次真实操作）。最后一次生产烟测位置由 `production-smoke-final.log` 的 `SMOKE_ARTIFACTS` 精确记录，包含六 EXE/两 DLL 的 payload SHA256、实际目录 timing 与 PNG；初次 `usability-c9ecab5e3a374a0f8aec16666f74136f/` 继续保留。

自动审批以 `blocked by policy` 拒绝清理四个本轮隔离测试目录（`ops-fs-fixture-run-1/2/3`、`ops-guard-fixture-run-1`）。未绕过拒绝，目录与诊断材料保留，不影响程序；具体命令和已核对无 reparse 的目录记录见 `ops-fixture-cleanup.log`。
