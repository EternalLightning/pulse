# 内容搜索与索引契约收口（2026-10-16）

范围为 U10、U11、R01、R02、R03。保留工作树已有修改；本次未操作实际索引服务。初次容量 fixture 存在用户配置读取隔离失误，具体证据及修正见末节；最终定向验证使用显式 fixture 数据库和语料。

## 修改与契约

- U10：transient 枚举在打开失败及非 EOF 的 FindNext 失败时保留错误。继续处理已枚举文件，发送已确认内容命中/重复组；最终 done=true、error 非零且返回 false。正常空目录不作错误，取消仍为 ERROR_CANCELLED。新增可选枚举 hooks 默认使用原 Win32 API。
- R01：非空 roots 覆盖兼容字段 root，与原客户端 wire 和 transient 分支一致；缓存和 task 使用同一契约。生产 wire 序列化/解析提取为 protocol 内联函数，生产入口与定向用例实际共用代码。旧 task 用例中 AND 交集断言按新统一契约更新，不删除用例。
- R02：共享 feed 逐条属性读取遇到非明确删除错误时标记本根不完整，阻止本根 epoch prune，保留旧缓存并公布 root.error。ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND 仍允许确认删除；恢复的完整扫描可正常 prune。采用根级保守保留，单文件失败期间该根的其它旧条目可能延迟清理。
- R03：普通 Reconcile 保存每根枚举错误，只有完整结束才 Ready；访问失败为 AccessFailed，其它非完整错误为 Recovering。共享 Reconcile 同样保存逐条失败。进一步核对 Impl::Error 会将 current_root 对应的错误写入 root.error，现有 app_runtime.cpp:481 内容文件夹投影已通过 root.error/available 显示不可用及错误码，原 UI 提示足够。因此 R03 不登记为缺失 UI 错误提示；本次只将非完整扫描的状态机语义与已有错误提示一致，避免 Ready 与错误并存。未修改 UI 代码。
- U11：维持最多 16 个有效查询，增加一个管道实例用于控制/拒绝超额请求。超额查询收到 done+ERROR_BUSY，已有查询保持连接；临时 ERROR_PIPE_BUSY 用 25 ms 父进程等待重试，停止标志可在下一轮退出。接受取消和 shutdown 的控制通路仍可使用。无请求数据的恶意/失常连接仍可能占用管道实例，这不是本次改变的协议超时范围。

## 定向验证入口

| 目标 | selector | 范围 |
| --- | --- | --- |
| pulse_content_search_test | --transient-errors | 正常双根、部分/全部缺根、重复扫描部分命中、非 EOF 注入、正常 EOF/空目录、取消 |
| pulse_content_search_test | --task-enumeration | 既有窄范围/枚举回归，并更新 roots 覆盖 root 用例 |
| pulse_content_index_test | --content-contract | 真实生产 wire 双根往返、缓存与 cache+task 扫描、单根兼容 |
| pulse_content_index_test | --root-errors | 注入子目录访问失败、每根状态、健康命中、恢复 |
| pulse_content_index_test | --shared-retention | 隔离 filename feed、暂时属性失败保留缓存、恢复更新、真实删除 |
| pulse_content_index_test | --agent-capacity | 隔离 agent 16 订阅、超额拒绝、控制取消/恢复、其它订阅存活、shutdown |
| pulse_content_index_test | --content-config-errors | 显式数据库 Instant/Writer 构造、坏 sidecar 拒绝、原始字节保留 |

主代理完成第一批构建后，本包检查二进制确实含对应 selector，再执行了以下初步检查（尚非最后源码状态）：

| selector | 初步结果 | 退出码 |
| --- | --- | --- |
| --transient-errors | 9 PASS / 0 FAIL | 0 |
| --task-enumeration | 11 PASS / 0 FAIL | 0 |
| --filename-query-boundaries（query 包共享检查） | 17 PASS / 0 FAIL | 0 |
| --content-contract | 6 PASS / 0 FAIL | 0 |
| --root-errors | 5 PASS / 1 FAIL | 1 |

初步 root-errors 失败为旧测试二进制尚未含恢复时强制重扫的配置变更；二进制中确认无新 `unused-recovery` 标记。Configure 相同配置不会主动安排重扫，故测试已改为追加无对应目录的 exclusion，要求新的完整扫描实际发生；需重建验证。初步 index 二进制没有 shared-retention/agent-capacity selector，未运行它们，避免未识别参数进入旧完整套件。

受限执行 content exe 无输出，Python 获取原始退出码为 3221225794（0xC0000142，DLL 初始化失败）；同命令通过 require_escalated 环境执行后正常产生上述输出。该失败按环境限制记录，不归因为产品回归。

最终获得主代理独占 build 槽后构建 `pulse_content_search_test pulse_content_index_test` 及其 CMake 必要依赖成功，无新增警告。`pulse_content_index_test` 增加对生产 content_agent.cpp 的链接，仅用于 child 入口复用同一生产 agent 实现。

最后源码状态批量运行结果如下；所有进程均退出 0：

| selector | 最终结果 |
| --- | --- |
| --transient-errors | 9 PASS / 0 FAIL |
| --task-enumeration | 11 PASS / 0 FAIL |
| --filename-query-boundaries | 17 PASS / 0 FAIL |
| --content-contract | 6 PASS / 0 FAIL |
| --root-errors | 6 PASS / 0 FAIL |
| --shared-retention | 12 PASS / 0 FAIL |
| --agent-capacity | 11 PASS / 0 FAIL |
| --content-config-errors | 6 PASS / 0 FAIL |

共 88 个通过断言。原始 stdout/退出码保存在 `bench_data/content-audit-closure-2026-10-16/green-*.log`，运行脚本同目录 `run_selectors.py`。此次验证没有运行旧完整套件。红对照尚未执行；为不阻塞其它包排队验证，按主代理要求先交还 build 槽，因此不声称已经在旧生产行为上动态复现所有候选。

额外配置生命周期集成检查与 host_config 包协调：Instant 构造使用相同 checked path loader；Writer 在坏 sidecar 时在打开数据库前停止，LoadOrCreateConfig 后续失败也先保存错误、清 writer_db 并关闭连接再退出。`--content-config-errors` 使用显式 fixture 数据库，验证两种构造不复活旧 SQLite roots，且坏 sidecar 与数据库原始字节均未改变。checked loader 本体及其独立输入测试归 host_config 包。

## 限制

异常枚举与暂时属性拒绝由窄 hooks 确定性注入；不代表真实 SMB/provider 中途断线、ACL 故障或生产吞吐验证。最终容量测试通过 content_index_test 的 child selector 传入显式 database_path，数据库、sidecar、命名 mutex 与 pipe token 均由隔离 fixture 决定；不依赖 LOCALAPPDATA 实现配置隔离。没有执行完整历史套件、selftest、实际服务控制或 UI 视觉检查。


## 初次容量 fixture 隔离失误与审计

初次容量 fixture 启动 Pulse.Index.exe 时只覆盖 LOCALAPPDATA，随后在初始化等待中超时，尚未建立 16 个订阅。进一步读取发现 ConfigurationPath 对默认数据库会调用 storage::ConfigurationRoot，它读取 KnownFolder/HKCU locator，LOCALAPPDATA 不足以隔离该 sidecar。此版本 fixture 已弃用，改用显式 database_path 的 test child；最终 11 项容量检查通过。

本次没有运行前的用户文件 hash，因此不能以事后 hash 断言绝对未写。实际只读审计证据如下：

- HKCU\Software\Pulse 的 StorageLocations 指向 configuration=`D:\AppData\Pulse`、index=`D:\AppData\Index`。
- `D:\AppData\Pulse\content-index.config` 为 100 字节，LastWriteTimeUtc 为 2026-10-01 05:43:59，SHA256 为 `907CF2624A1C5C1EE6B7646D9B286FFCDBCCEAF46902A2AEB24697D2A503403B`。
- `C:\Users\EternalLightning\AppData\Local\Pulse\content-index.config` 同为 100 字节及同一 hash，LastWriteTimeUtc 为 2026-10-01 04:55:55。
- 上述时间均早于本次 2026-10-16 运行，支持本次未重写这两份配置；没有盲目恢复任何用户文件。初次数据库路径受 LOCALAPPDATA 覆盖，位于已经清理的隔离 fixture，未使用真实用户数据库。实际 LocalAppData\Pulse\ContentIndex 未枚举到数据库文件。
- 初次初始化超时的具体状态未在旧 fixture 清理前捕获，不据此推断根因。可能读取用户配置这一隔离缺陷本身已确定并修复；没有把该失败列为产品容量回归。
