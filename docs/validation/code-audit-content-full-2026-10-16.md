# Content agent / transient / task search 全文静态审查（2026-10-16）

## 范围与基线

本包静态审查基线覆盖七个生产文件，合计 1,845 行；先检查工作树与目标 diff。初始检查时七文件均没有相对 HEAD 的 diff，仓库其它大量未提交修改保持原样。随后主代理授权 CF-01/CF-02 的窄修复，实施与验证状态见末节；CF-03/CF-04 没有修改。本文发现行号指静态审查基线。

`content_search_protocol.h` 实际位置是 `src/index/`，不在 `src/ipc/`。本报告仅对本次读取时的静态控制流作结论，不把已有测试源码等同于本次测试通过。`content_index` 与 client 主体由主代理负责；这里只为明确边界读取必要调用段。

## 可行动发现

### CF-01 / P2：窄范围 task 查询仍遍历其它配置根和无关子树，并受其失败影响

- 位置：`src/index/content_task_search.cpp:451-457,515-557,568-570,583-585,648`。
- 触发：配置含正常根 A 与已断开的根 B，查询只指定 A；或配置根 base 下有 wanted 与访问拒绝/离线的 unrelated 目录，查询 root=base\\wanted。无需在无关目录内存在任何匹配文件。
- 依据：`visit` 对文件执行 request.root/roots/recursive 过滤，但 DFS 根列表来自全部 `config.roots`，`open` 只检查 excluded 和 SystemPath，没有查询范围相交判断。遍历普通目录时也没有根据 request.recursive 剪枝。无关根/目录打开失败会设置 `complete=false` 和错误码，最终 total_files=0 且函数返回 false。
- 影响：可读的窄范围查询被范围外错误标为不完整；非递归/深层小目录查询还会枚举整个配置语料，等待无关设备 I/O。这个问题与 CF-02 不同：这里失败被报告，但本不应访问该范围。
- 修复方向：在打开配置根、进入目录前判断其子树是否与有效查询范围相交；保留通往深层查询根的祖先。文件仍使用现有完整范围检查；非递归查询不应走入查询根的子目录。不要简单要求每个目录已在查询根内部，否则会重现祖先被错误剪枝的问题。
- 定向验证建议：配置一个正常根和一个不存在的无关根，仅查询正常根，要求返回完整结果且无路径错误；在单根中设置 wanted 与不可枚举 sibling，要求只访问 wanted。加非递归和多个不重叠 request.roots 的边界。

### CF-02 / P2：task DFS 把中途枚举错误当作完整结束

- 位置：`src/index/content_task_search.cpp:562-565,583-585,607,648`。
- 触发：目录已成功 FindFirstFileExW，但后续 FindNextFileW 因 SMB/provider 断线或其它 I/O 错误失败，而错误不是 ERROR_NO_MORE_FILES。
- 依据：任何 FindNextFileW=false 都直接 FindClose/pop，没有读取和分类 GetLastError。只要其它路径正常，`complete` 保持 true，最终报告 total_files=已见数且返回 true，progress.error 可仍为 ERROR_SUCCESS。
- 影响：漏掉后续候选，但呈现为完整扫描。不能由现有状态判断这些文件实际不存在；上层协调/替换结果时必须能识别不完整扫描。
- 修复方向：在失败后、FindClose 之前立即保存 GetLastError，仅 ERROR_NO_MORE_FILES 表示正常完成；其它错误设置 complete=false 并保留诊断。不要将取消导致的错误覆盖成成功。
- 定向验证建议：为目录枚举增加最窄可控 seam，在首条成功后注入 ERROR_BAD_NETPATH / ERROR_READ_FAULT，并保留正常 EOF 对照。实际 SMB 断连验证另记，普通本地 fixture 无法可靠制造这个时序。

### CF-03 / P2：transient scanner 在部分根不可读时仍报告成功

- 位置：`src/index/content_search.cpp:289-305,334-346`，完成路径 `:565-581,640-643`。
- 触发：request.roots 至少一个可枚举根，另一个不存在/拒绝访问；或成功打开根后无法打开其中一个应扫描子目录。此分支服务明确 transient 内容搜索和重复文件搜索。
- 依据：打开失败只更新局部 last_error；仅当整个请求从未打开过任何目录（!opened）才写 progress.error 并返回失败。打开过一个目录后，其他失败不再影响最终成功。FindNextFileW 的非 EOF 错误同样未分类。
- 影响：部分结果与完整结果不可区分；重复文件扫描可在漏掉整棵树时仍报告成功结束。这里不是要求访问拒绝文件必须有结果，而是应报告扫描范围未完整覆盖。
- 修复方向：累计范围内枚举错误并传入最终 progress，明确完整性/返回值契约；保留其它根的已确认命中。目录中途失败按 CF-02 分类。显式 candidate_paths 的属性失败目前也被跳过，其是否需要逐候选错误应在同一契约下决定，不凭本次审查扩张行为。
- 定向验证建议：使用隔离目录 A 与确定不存在的根 B，无需 ACL 或网络即可复现：roots={A,B}，A 放匹配文件，要求保留 A 命中且最终声明 B 的错误/不完整；另测全部根不存在与正常多根。

### CF-04 / P2：达到 16 个长期管道连接后，agent 自行退出并取消已有查询

- 位置：`src/index/content_agent.cpp:244-247,303-305`。
- 触发：同一 persistent agent 已接受 16 个仍存活的查询/订阅连接。客户端必要调用段 `content_search_client.cpp:315-337` 为每个待处理请求创建独立 ActiveQuery，未在该层施加 15 个连接上限。
- 依据：CreateNamedPipeW 将最大实例数固定为 16。接受第 16 个仍存活的连接后，accept loop 下一轮再次创建第 17 个实例；失败分支无错误分类地 break，随后设置 stopping、取消 sessions 并 join 所有处理线程。并不需要第 17 个客户端先发来请求。
- 影响：已有 16 个有效订阅被整体中断；非 Instant client 的自动重连可能再次达到同一阈值。尚未验证应用 UI 可同时构造多少个 session，因此不宣称正常用户流程必定达到 16；agent/client 的容量边界本身是静态可达的。
- 修复方向：容量耗尽应等待/回收空闲 handler 或采用明确的 admission 策略，并保留控制请求能力；不能仅在客户端限制后继续将暂时实例耗尽解释成全局关闭。若改为更多实例，也应定义上限与资源预算。
- 定向验证建议：直接建立 16 个长期订阅，等待第 16 个就绪，断言原连接与 agent 仍存活；再核对取消、状态与父进程退出仍可完成。该项本次没有执行。

## 逐文件覆盖

| 文件 | 实际已读范围 | 静态检查重点与结论 |
| --- | --- | --- |
| `src/index/content_agent.cpp` | 全文 1-310 | 请求字段与 payload 上限、token/当前用户 pipe ACL、单次/persistent agent、generation 取消集合、订阅 revision 等待、父进程监控、同步 I/O、handler 回收。CF-04。同步 Read/Write/Flush 依赖客户端消费/断开与 shutdown CancelSynchronousIo，不能由原子 cancel flag 单独中断；本次未将其推断为额外已复现死锁。 |
| `src/index/content_agent.h` | 全文 1-13 | 入口重载及 bool read_only 到 ContentAgentMode 的映射；无新增可证明问题。 |
| `src/index/content_search_protocol.h` | 全文 1-113 | 8 MiB 入站上限；config roots/exclusions/encoding/最大文本与文档字节边界；兼容可选字段、状态/revision/change sequence、subscription error 与 failure 成对校验。GetConfig/GetStatus 是组合载荷解析器，不能在内部一律要求 remaining=0，因为 status 后继续附带 config；不将其误报为尾随数据缺陷。 |
| `src/index/content_search.cpp` | 全文 1-646 | 大小写/词边界/词组/排除词、分片匹配取消、snippet/line、路径长前缀、候选属性、DFS/reparse/system 跳过、sample+SHA256、hardlink 身份、hit cap、排序和完成回调。CF-03。重复文件 sample 和候选装载阶段缺少逐项原子取消检查，完整 hash 有检查；未测网络阻塞取消延迟。文件变化前后身份/内容一致性不强于当前代码，未宣称扫描是某一时刻的原子快照。 |
| `src/index/content_search.h` | 全文 1-94 | request/progress/hit 的默认值、maximum_hits=0、paged_results/previous_results 本地字段、subscription 错误语义；无新增可证明问题。 |
| `src/index/content_task_search.cpp` | 全文 1-650 | 三 lane 工作数/借用、PDF 公平队列、8 MiB/lane 内存预算、1 GiB 总 spill 写入预算、payload 大小、临时文件 delete-on-close、互斥区/结果队列、metadata/version 复核、reader 取消、feed 游标、DFS、异常与 join 顺序。CF-01、CF-02。32 个待发命中有背压；回调仅在调用线程；cancel/拒绝/hit cap 均走 join.Finish。版本复核不能证明 metadata 首读与后续版本读取属于同一文件状态，尤其非 literal 文档路径；该竞态列为静态限制，未制造并发替换证据。 |
| `src/index/content_task_search.h` | 全文 1-19 | cache/version/fresh、reader 注入、调用线程回调与不保留正文契约；无新增可证明问题。 |

## 必要调用者与已有测试核对

- `content_index.cpp:1431-1469,1603-1612`：SearchTask 使用 config 快照，deliver 跟踪 callback accepted，最终返回 completed && accepted。因此 supplement 最终 callback 的返回值未直接使用，不足以单独证明生产路径忽略下游拒绝；本报告没有把该点列为 bug。
- `content_search_client.cpp:48-80,173-193,315-337,428-467`：核对当前写出的请求字段布局、会话取消、并发请求派发、读取 8 MiB 边界、generation 核验与断连重连；不重做主代理的完整 client 审查。
- `content_scope.h:8-18`、`index_query.cpp:343-398`：仅核对 scope 前缀/分隔符与 filename query 的解析契约。没有修改已有 type/kind 否定修复。
- `src/bench/content_search_test.cpp:110-279,280-389`：已有 transient 内容多编码、词模式、候选、whole word、非递归、hit cap、预取消、hardlink/空文件重复、最低大小、系统跳过、两根重复、junction 循环等源码用例。它们没有覆盖部分根缺失的完整性声明。
- `src/bench/content_index_test.cpp` task fairness/balance/metadata 与 SearchTask 相邻用例：存在候选去重、lane 调度、取消回收、metadata 不阻塞枚举、缓存+增补、cap、重叠 roots 测试；本次只读相关代码，没有执行。已读的覆盖不包含窄请求避开坏的无关配置根，也未注入 FindNextFileW 中途错误。
- 核对已有 `docs/validation/code-audit-small-content-2026-10-16.md` 与 folder-kind 否定报告，仅避免重复登记此前目录协调与解析修复；其测试结果不是本报告本次执行结果。

## 实际验证与限制

静态阶段实际运行的是 git status、七文件 git diff 检查、rg 定位与必要源码读取，以及本报告的 diff/空白检查。上述四项发现均来自静态控制流。

主代理授权后，CF-01/CF-02 的生产变更限定在 `content_task_search.cpp/.h`：DFS 在打开目录前应用请求范围相交过滤，保留祖先；FindFirst 的 ERROR_FILE_NOT_FOUND/ERROR_NO_MORE_FILES 作为空目录，其他打开错误保持不完整；FindNext 的异常错误在关闭 handle 前保存并向最终 progress 传递。保留 request.root 与 request.roots 同时存在时的原有 AND 语义。新增可选枚举 hooks，默认直接使用原 Win32 API。

定向用例放在 `src/bench/content_task_enumeration_cases.h`，由 `pulse_content_search_test --task-enumeration` 在原 fixture/用户配置访问之前独立进入。用真实隔离目录、真实目录 handle 与注入 reader，枚举 hooks 固定空目录和故障错误码并记录打开路径。覆盖窄范围跳过无关坏根/兄弟目录、非递归不进入子目录、深查询根的祖先保留、多 roots、root 与 roots 的交集、两个正常空目录错误码、访问拒绝、中途枚举错误保留命中与正常 EOF。

经主代理明确授予串行构建槽，实际执行如下：

```powershell
cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_content_search_test"
.\build\pulse_content_search_test.exe --task-enumeration
```

| 阶段 | 构建 | 定向结果 | 退出码 |
| --- | --- | --- | --- |
| 红测：保留 hooks 的原 DFS 行为对照 | 同目标成功 | 3 PASS / 8 FAIL | 1 |
| 绿测：恢复完整修复 | 同目标成功，日志确认重新编译 content_task_search.cpp | 11 PASS / 0 FAIL | 0 |

红测对照并非未经修改的旧二进制：保留新增 hooks 与函数参数，仅移除本补丁的范围相交过滤、恢复 FindFirst 任意失败均不完整、恢复 FindNext 任意失败均作正常结束，以便同一批确定性故障用例复现原行为。红测失败对应窄范围、非递归、深祖先、多 roots、AND 交集、两种空目录、异常 FindNext 共八项；fixture、访问拒绝、正常 EOF 三项对照通过。随后从精确备份恢复绿测源码并刷新时间戳，构建日志确认编译器实际重编译本次生产文件，没有复用红测对象。两个阶段均使用已知可避开 sandbox VerifyGlobs 停滞的 `require_escalated` 环境；本次没有另行触发 sandbox 停滞。

红测 fixture 保留在 `bench_data/content-task-enumeration-34880-10588484` 便于检查；绿测成功 fixture 自行清理。`git diff --check` 对三个已跟踪修改文件通过。未运行旧 content 全套、selftest 或 UI 检查；构建目标的必要依赖由 CMake 自动处理。

另有待主代理核对的请求契约：wire parser 会设置 root=首个 root 且 roots=全部 roots，而 task 文件过滤同时要求位于 root 和 roots；额外不相交 roots 因此可能被排除。本补丁按授权保留原语义，不擅自扩大缓存/扫描查询范围；需结合 cached search 与调用者另行决定统一的有效 roots 契约。

没有进行真实 SMB/provider 断连、16 订阅容量、受限 ACL、磁盘 spill 故障、agent 资源耗尽、UI 主题/DPI/布局或吞吐测量。异常枚举、访问拒绝与空目录返回码由 hooks 确定性注入；绿测验证生产 DFS 的边界/错误分支，不能替代真实外部文件系统故障验证。没有对外部 Win32 文档做本次网络查证，管道容量结论应由建议的实际容量用例最终确认。其它模块和现有未提交更改均不在本包写入范围。
