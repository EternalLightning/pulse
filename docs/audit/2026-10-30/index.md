# src/index 全文静态审查

审查日期：2026-10-30。范围：`src/index` 当前全部 120 个文件（包括未跟踪的 `usn_record.h`）；已先读仓库规则及本分区当前 diff，再逐文件全文读取。覆盖明细见 `index-coverage.json`。保留全部已有修改；本工作包只写本报告和覆盖清单，没有修改产品源码、构建、启动 helper/service、运行测试或更改用户配置。

发现 6 项 P2；没有依据静态代码确认 P1。下列“已确认”指控制流、数据流或复杂度已由当前源码确认，不等同于已完成 Windows/SMB/大规模运行复现。行号对应当前工作区。

## IX01 / P2：多目录即时内容订阅只接收首个目录的变化

- **位置**：`src/index/content_instant_session.cpp:101-108`；协议依据 `src/index/content_search_protocol.h:28-35,124`；初始搜索依据 `src/index/content_task_search.cpp:454-458`。
- **触发**：请求 `roots={C:\A,D:\B}`，启用即时内容搜索订阅，初始扫描完成后在 B 创建、修改、删除匹配文件，或移动一个包含匹配文件的目录。
- **证据（静态已确认）**：协议把 roots 首项写入 legacy `root`，解析后同时保留 roots 数组。初始扫描按 roots 数组的任意成员接纳文件；订阅 `included` 却只判断 `Under(path, request.root)`，`ancestor` 同样只检查该首根。所有 watcher/feed 事件都先经过 enqueue 的上述判断；B 的事件在进入队列前即被丢弃。
- **影响**：初始结果可以含 B 的文件，后续 B 的新增结果缺失，已删除或不再匹配的结果继续显示；状态仍显示订阅正常，不会触发队列溢出或 gap 提示。
- **建议**：抽取统一请求范围谓词，roots 非空时以数组为准，legacy root 仅作后备；祖先事件同样对任一请求根判断。`content_hit_aliases.cpp:95-97` 也同时要求 legacy root 与 roots，是同一兼容字段分叉应一并核查的调用点。
- **定向验收建议（未运行）**：双根分别覆盖创建、删除、编辑、跨目录重命名、包含子树的目录移动；对 shared feed 和独立 watcher 各验证一次，保证初始与增量范围一致。

## IX02 / P2：逐条任务结果让落盘显示顺序产生二次方重建

- **位置**：`src/index/content_task_search.cpp:636-641` → `src/index/content_search_session.cpp:29-30` → `src/index/content_result_store.cpp:231-280`，核心在 `270-272`。
- **触发**：即时/任务内容搜索命中大量文件，使用 paged result store（包含默认索引顺序）；缓存正结果也在 `content_index.cpp:1578-1582` 逐条发送。
- **证据（静态已确认）**：任务汇总循环每发现一个 hit 就以单元素 batch 调 callback；session 对 task_scan 每次调用 StreamUpsert。UpsertChanges 持有 IO mutex，针对所有 hits 执行 `row_number() OVER(ORDER BY ...)` 写入完整新表，再 DROP/RENAME visible。即使排序索引已存在，也必须枚举并写入当前全部 N 行。累计 N 次单条插入需要约 N(N+1)/2 次显示行写入，另有事务、计数和缓存失效成本。
- **影响**：结果越多扫描消费越慢，后续解析结果受到队列反压；翻页、排序和依赖同一 IO mutex 的操作会等待这些重建。10,000 条命中就会累计写入约 5,000 万个显示位置；实际耗时和磁盘写入量未测。
- **建议**：按数量/短时间窗合并结果批次并节流可见顺序更新，或维护可增量插入的显示顺序结构。保留首条结果及时可见、全局排序正确和完整计数，避免仅把瓶颈挪到 UI。
- **定向验收建议（未运行）**：1k/10k/100k 命中，记录每次重建/事务次数、扫描完成时间、首屏/翻页延迟；同时验证用户中途切换排序和过滤。

## IX03 / P2：旁路本地索引客户端遗漏现有服务身份验证

- **位置**：`src/index/index_feed.h:35-41,64-70`；`src/index/change_tracking_client.cpp:234-242`。对照 `src/index/index_client.cpp:103-111` 与 `src/index/index_server_auth.h:33-54`。
- **触发**：真正的 PulseIndex service/helper 未持有端点时，另一本地进程抢先建立同名 `\\.\pipe\PulseIndex` 并返回格式合法的 feed、folder-size 或 change-tracking 响应。
- **证据（静态已确认）**：主 IndexClient 在 CreateFile 成功后执行 VerifyIndexServer，按 sibling helper 图像/令牌身份或 SCM 登记的 LocalSystem service PID 验证；IndexFeedConnection 两条连接路径和 ChangeTrackingClient 的本地 Worker 直接开始交换消息。相同变更客户端的 network 分支 `change_tracking_client.cpp:170-178` 已验证服务器，显示此处接口分叉遗漏。服务端 `index_host.cpp:742-762` 的 FIRST_PIPE_INSTANCE 只使正规服务在抢占发生后无法创建端点，不能替这些客户端认证当前连接。
- **影响**：这些功能会接受主搜索客户端本应拒绝的伪造服务器数据；请求的本地路径也会发送给该进程。这里不声称已证明权限提升或跨用户利用；实际 Windows 令牌/DACL 场景尚未运行验证。
- **建议**：复用同一认证连接工厂，在任何业务请求前验证；保留显式测试 fixture 端点的受限注入方式，避免生产路径为测试方便跳过认证。
- **定向验收建议（未运行）**：自有测试 pipe 的错误映像/错误身份服务器须被拒绝；正规 session helper 与已登记 service 分别保持可用，取消连接仍及时结束。

## IX04 / P2：网络索引遗漏不可用根后仍返回完整成功

- **位置**：`src/index/network_index.cpp:1174-1177,1252-1285`；根失败状态 `996-1002`；路由 `src/index/network_agent_client.cpp:368-377`；默认质量字段 `src/index/index_types.h:35-41`。
- **触发**：搜索配置过但离线、首次构建尚未完成或同步失败的网络根；也可以是多个根中只有一部分有可用 shard。
- **证据（静态已确认）**：SearchLoop 只复制 `root.info.online && root.shard` 的根，丢弃其余根；创建默认 SearchResult 后只填 total/hits，不把 root.info 的离线、building、失败或旧索引状态写入 error/incomplete。UsesLiveSearch 只检查路径是否属于已配置根，不看根可用性，因此不会自动转 live search。
- **影响**：配置根未被查到的文件与真正“没有匹配”在查询响应上完全相同；多根查询返回静默缺项，旧 shard 同步失败时也无法从响应识别陈旧质量。根设置页虽然有状态文字，查询响应本身仍宣称完整。
- **建议**：只对与请求范围重叠的根汇总质量，传播 incomplete 及适当错误/扫描状态；若产品选择只查在线 shard，应明确在查询结果上显示缺失覆盖。不要因无关离线根使局部查询误报不完整。
- **定向验收建议（未运行）**：已配置根离线、无首个 shard、旧 shard 同步失败、双根其中一根失败、查询仅限健康根；验证响应质量字段及 UI 能区分完整空结果和缺失覆盖。

## IX05 / P2：启动补追路径绕过有界 USN 队列并无限累积积压

- **位置**：`src/index/index_engine.cpp:2976-3022,3045-3078`；启动调用先后 `3732,3790`；对照有界收集器 `src/index/usn_stream.h`。
- **触发**：管理员/service 从有效缓存启动，磁盘 USN 自 checkpoint 后有大量积压；或者读取期间持续有足够多文件变更。
- **证据（静态已确认）**：Worker 启动时先 CatchUpVolume，之后才 StartJournalStreams，所以此时必走没有 stream 的同步分支。该分支反复 FSCTL_READ_USN_JOURNAL，把每条记录 append 到同一个 blob，直到一次返回零条；既没有字节/记录上限，也没有捕获有限结束 USN。只有全部读完才取得引擎写锁并应用、更新 checkpoint。已存在的 per-stream 队列上限与分批 Take 对这条启动路径不起作用。
- **影响**：启动内存随全部未应用日志线性增长，单卷长时间补追会延迟后续卷及维护；持续写入可能使此轮追赶长时间不结束。超大分配失败的进程行为和实际峰值未运行验证，报告不把潜在 OOM 当作已复现崩溃。
- **建议**：补追也使用有界批次与有限目标 USN；每批校验后按一致的提交边界应用/checkpoint，并允许取消、通知和其他卷推进。不要简单丢弃溢出记录或在未应用时推进 checkpoint。
- **定向验收建议（未运行）**：注入大量合法 USN 批次及持续追加源，观察最大暂存字节数有界、可取消、每卷公平推进，以及中途失败后重启不漏变化。

## IX06 / P2：MFT 系统枚举后备路径把已有文件元数据写成零

- **位置**：`src/index/index_engine.cpp:3138-3185,3295`；默认值 `src/index/index_engine.h:159-164`；属性查询消费 `index_engine.cpp:910-918,1524-1530`。
- **触发**：EnumerateMft 返回 Failed 后，FSCTL_ENUM_USN_DATA 后备枚举成功。一个源码内可见条件是 `index_mft.cpp:308-310` 检测原始 MFT 的已解析 run 映射未覆盖有效长度；也包括底层原始读取失败。
- **证据（静态已确认）**：FrnNode 的 size/mtime 默认为 0；正常 EnumerateMft callback 在 3124-3125 填充两值，后备循环只填 frn/parent/name/is_dir，没有获取 size/mtime 或标记属性覆盖缺失。BuildMftTree 把这两个零写入持久 Attr。后续仅应用 checkpoint 之后的 journal；没有全卷补齐步骤。查询仍直接用该 Attr 做大小/时间过滤并返回给 UI。
- **影响**：未再发生变化的已有文件会显示零大小/未知时间，正大小或日期查询漏报，零大小查询误报，目录大小汇总也缺失；文件名搜索看似正常，无法从成功状态得知属性不完整。
- **建议**：后备枚举后有界补齐文件属性，或维护属性有效性并将依赖属性的查询标为不完整；不能把 unknown size 当作真实 0 字节。对读取失败保持明确覆盖状态。
- **定向验收建议（未运行）**：强制原始 EnumerateMft Failed、后备枚举成功，放入不同大小/时间的稳定文件；验证显示、size/date 条件和目录汇总，另覆盖属性无法读取的情况。

## 设计与边界记录

- 多目录范围在协议、task scan、instant subscription、hard-link alias 中各自实现；IX01 是已确认行为分叉。建议先统一范围语义与兼容字段优先级，再讨论抽象，不建议为了形式重构整个搜索系统。
- 本地/网络/内容查询复用部分协议，却分别实现连接生命周期和认证；IX03 是已确认遗漏。应统一能保证身份与取消的连接入口，同时保留各服务器已有响应与权限语义。
- ChangeTracker::Lease 当前恒不安排 baseline，Seed 为 no-op；Engine 和 NetworkIndex 的 seed 队列、扫描 baseline 路径仍存在。静态调用链显示维护负担，但历史 InitialMtime 记录仍可能需要读取兼容。仅建议单列清理设计评估，未把无运行影响的遗留代码列成缺陷或删除建议。
- 已确认普通运行 USN collector 有容量约束、任务元数据/结果队列有约束、文档解析有独立进程限制；这些局部约束不能消除 IX02 和 IX05 的其他路径成本。
- app 核心代理负责“异步选择按旧显示行号回写”问题；本模块确认 ContentResultStore::Match 只回传整数位置且没有返回 epoch（493-510）。本报告不重复编号该跨模块问题。
- 按任务边界，**未把服务文件名搜索有意允许发现无打开权限的名称列为漏洞**。

## 实际验证

- 已完成 120/120 文件全文静态阅读与本分区 diff 阅读，包含新 USN 校验头；覆盖 JSON 是路径数组，不是仅凭搜索或 hash 推算的阅读记录。
- 仅进行了读取、定位和报告/JSON 静态核对；没有构建、测试、性能测量、SMB 故障注入、命名管道伪造或真实用户配置写入。
- 报告中的验收均为后续定向验证建议，未运行的检查不记为通过。
