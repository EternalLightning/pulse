# Content 小文件批次静态审查（2026-10-16）

## 范围与基线

第一阶段只读审查生产代码与现有测试，审查前已读取仓库 AGENTS.md。工作树存在大量并行未提交修改；下列八个目标文件在初始读取时均无相对 HEAD 的 diff。本报告不重复登记 `content_result_store` 已处理的 pending epoch 问题。主代理复核后授权的修改与验证见末节；没有修改 CMake、提交或发布。

以下发现及行号记录修复前静态审查基线；修复后新增回归已用 hooks 和真实隔离目录复现两项控制流问题。真实 ReadDirectoryChangesW / feed 事件形态与端到端 UI 后果仍未动态验证。

## 发现

### SC-01 / P2：同路径目录替换被合并后，旧子孙命中没有被删除

- 位置：`src/index/content_instant_session.cpp:109-110` 的 `enqueue` 合并，以及 `:216-237` 的目录处理；特别是 `:220-223` 只在当前路径不存在时清除 `matched` 子孙，而现存 imported 目录分支只收集现存文件。
- 可达条件：一次已有匹配的目录被移出查询范围，另一个目录很快导入相同路径，两个同路径事件在 `:199-201` 的 150 ms 窗口内合并。合并只保留最终 path 和 imported=true。处理时该路径已经是新目录，所以不会经过删除子树分支。
- 静态结果：例如 `matched` 已有 `C:\base\slot\old.txt`，最终 slot 只含 `new.txt`；重扫只为 new.txt 发增量。old.txt 不在 `files` 中，不会走 `:258-259` 的单文件非匹配删除，且 `matched` 中的旧行没有其它清除路径。订阅继续 live，不会主动重新全扫来纠正。
- 现有保护：路径本身仍不存在时会删除所有旧子孙；单独收到 old.txt 的删除事件时也会删行。并非每次目录替换必现：如果传输额外提供每个旧子孙事件，或者 move-out 已在另一轮完成处理，这些保护可消除旧行。但当前消费逻辑没有保证必须收到子孙事件，也没有保存目录消失这一历史状态。
- 最小复现建议：隔离 fixture 配置根 `C:\fixture\base`，初始回调发布 `base\slot\old.txt`。在初始完成后，将 slot 整体移到配置根之外，将仅含匹配 new.txt 的另一目录移入 slot，在同一合并窗口投递 slot 的移出与 imported=true 通知；只投递目录通知。要求最终先删除 old.txt 并新增 new.txt，结果恰为一行。可先用 `ContentInstantSessionHooks` 固定相同 pending 状态，再补真实 watch 用例。
- 修复建议：对被替换/导入目录做局部子树 reconciliation：保留受影响的旧匹配集合，以已验证存在且仍匹配的文件重新确认，删除已确认消失或不再匹配的旧行。目录遍历或读取失败不得直接当作不存在；维持已有错误保留语义。不需要重新扫描整个配置范围。
- 现有测试缺口：`src/bench/content_instant_test.cpp:247-259` 覆盖单文件新增/修改/改名/删除与避免重扫兄弟目录；`:314-329` 只覆盖祖先移出后的旧行移除，没有同路径目录替换合并。

### SC-02 / P2：深层查询根的祖先导入被接受后，遍历在到达查询根之前被剪枝

- 位置：`src/index/content_instant_session.cpp:97-105` 的 `included` / `ancestor` 判定，以及 `:229-235` 的 imported 目录遍历，核心为 `:233` 的 `!included(child)` 即 `disable_recursion_pending()`。
- 可达条件：配置根是 `C:\base`，查询根是 `C:\base\parent\mid\leaf`；向原位置导入/改名恢复 `C:\base\parent`，其内部含 `mid\leaf\match.txt`。事件路径 parent 是查询根祖先，`:103-105` 会刻意接纳。但遍历首先遇到 parent\mid，它还不是查询根内部，`included` 为 false，因此立即停止进入 mid，永远到不了 leaf。
- 静态结果：目录已经恢复且存在匹配文件，订阅仍 live，但不发新增行；需要独立的后续文件事件或显式重搜才能补齐。祖先距查询根仅一级时，首个 child 已是查询根，可能正常工作，因此测试必须至少保留一个中间祖先目录。
- 现有保护：初始完整扫描的 scope 过滤会保留边界；enqueue 已明确允许查询根祖先；目录不存在时的删除使用 `Under(matched_path, changed_path)`，可以正确删旧行。这些保护均没有让 imported 枚举穿过仍位于查询根上方的中间目录。
- 最小复现建议：在隔离配置根内建 parent\mid\leaf\match.txt，查询 leaf 并等初始完成。把 parent 整体移出配置根，等旧行删除；再整体移回 parent，只投递 parent imported 通知。要求 live 增量重新出现 match.txt。用 hooks 可先确定性投递目录通知，再执行真实 DirWatch 回归。
- 修复建议：目录的遍历许可应使用“已在查询范围内，或是通往查询根的祖先”的相交判断；文件仍使用完整 included / 后续 SearchTask 的请求范围检查。保留配置 excluded/internal/reparse 限制，不能简单取消所有剪枝。
- 现有测试缺口：`:314-329` 的祖先改名用例只等移除后立即 Stop；没有移回，更没有查询根上方至少两级的导入场景。

## 逐文件覆盖

| 文件 | 已读范围 | 结论 |
| --- | --- | --- |
| `src/index/content_hit_aliases.cpp` | 全文 1-135 | 阅读原始路径发布、别名过滤、去重、取消和 Win32 handle RAII。全仓源码引用搜索仅发现本文件/头文件定义，CMake 也没有列入该 cpp，因此没有将其潜在行为作为当前产品可达缺陷。别名有文件 ID、volume、size、mtime、ChangeTime 复核和排除范围检查；枚举错误按可选补充处理，callback 异常传播。 |
| `src/index/content_hit_aliases.h` | 全文 1-22 | config/request 借用引用和单线程 emitted 集合契约；当前无生产调用，无法验证调用方生命期。 |
| `src/index/content_instant_session.cpp` | 全文 1-274 | SC-01、SC-02。检查 arming-before-scan、4096 路径上限、同路径合并、gap sealing/reconciliation、取消通知、collector/watch/monitor 回收及 delta 转发。未发现另一项可证明的 generation 或 join 生命周期缺陷。 |
| `src/index/content_instant_session.h` | 全文 1-17 | hooks 是回归注入接口，生产默认使用实际 feed/watch/scanner。没有新发现。 |
| `src/index/content_search_session.cpp` | 全文 1-58 | generation=0 / mismatch / 重复非 delta 完成有拒绝保护；paged delta 汇总在 done 后应用，error 时不应用；subscription_error 不替换扫描行；previous 只在非 task_scan 使用，失败/截断保留旧结果。没有新发现。 |
| `src/index/content_search_session.h` | 全文 1-31 | 逐请求 accumulator、previous/results 共享所有权、changes 生命周期；没有新发现。 |
| `src/index/content_scope.h` | 全文 1-48 | 读取路径分隔符/尾斜线/大小写 key、前缀路径边界、排除目录、共享卷范围与排除路径去重。根前缀必须有目录边界，防止 foo 误匹配 foobar。未做非 ASCII locale、长路径前缀与真实 UNC 实测，不能由本次静态检查宣称支持全部路径变体。 |
| `src/index/content_sort_sql.h` | 全文 1-26 | ordinal collation、filename/extension SQLite UDF；未发现新增可证明缺陷。CompareStringOrdinal 失败降为相等与超大 SQLite 文本等边界没有动态验证。 |

## 必要调用者与测试核对

- `src/index/content_search_client.cpp:390-461`：调用前建独立 ContentSearchSession；解析后的 generation 另有检查；取消/current generation 约束 transport failure 与重连。
- `src/index/content_agent.cpp:43-126,235-279`：请求字段解析、generation session 取消、instant 生产入口。
- `src/index/content_index.cpp:1431-1488`：SearchTask 的 config 快照、初始 progress、deliver 的 accepted/cancel 处理；未发现“忽略 search 返回值即可无错误删除”的独立可证明路径，因此未作为发现。
- `src/index/content_task_search.cpp:27-41,223-247,438-558,600-648`：候选仍受 request/config scope 检查，扫描错误通过最终 progress 回传；未对不属于本包的整个 task scheduler 重新做全面审查。
- `src/fs/fs_watch.cpp:190-270` 及 rename/old_name 解析符号：只核对父目录 watcher 的通知与关闭契约；未审查全部 watch 实现。
- `src/bench/content_instant_test.cpp`：读取 gap/取消 hooks、已有真实文件生命周期测试和祖先改名相关段；已覆盖断连、overflow、已知 pending 先协调后暂停等场景，未覆盖上述目录导入边界。
- `src/bench/content_refresh_test.cpp:160-213,230-265`、`src/bench/content_paging_test.cpp:55-78`：核对已有 stale generation、失败保留旧结果、成功空结果替换、task stream 排序去重场景。

## 经主代理复核后的修改与实际验证

- SC-01 已修复于当前 `src/index/content_instant_session.cpp:240-255`：只对 imported 目录下的旧匹配逐个读取属性，且仅在明确得到 `ERROR_FILE_NOT_FOUND` / `ERROR_PATH_NOT_FOUND` 时发 removed。枚举不完整、权限错误本身不删除旧行；存在文件内容变为非匹配仍走原单文件 SearchTask。
- SC-02 已修复于当前 `:97-108,236-239`：抽出配置范围检查与查询根祖先判断，目录可穿过通往查询根的中间祖先；无关兄弟、排除目录、reparse 边界保持过滤，文件仍必须通过 included。
- 新增 `src/bench/content_instant_test.cpp:42-124` 目录专项 fixture，通过现有 gaps 入口和额外环境筛选运行，不改测试 main 或 CMake。实际创建并改名隔离目录，使用实际 SearchTask 读取文件；hooks 只固定目录级通知和会话封口。覆盖旧子孙消失、已有路径内容变成非匹配、新文件新增、深层祖先导入、无关兄弟与排除目录不进入候选，并断言只有一次初始全扫。日志和配置都位于该进程独有的 bench_data fixture 下。
- 构建命令：`cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_content_index_test"`。首次 sandbox 构建在 glob 阶段停滞，终止本次自有构建进程后，使用同命令的 `require_escalated` 执行成功。不能将该停滞归因于代码。旧代码基线和修复后均成功编译目标及其必要依赖。
- 唯一功能运行命令：PowerShell 设置 `$env:PULSE_TEST_INSTANT_GAPS='1'; $env:PULSE_TEST_INSTANT_DIRECTORIES='1'; & .\build\pulse_content_index_test.exe`，在相同 `require_escalated` 环境执行。**旧代码 3 PASS / 3 FAIL，exit 1；修复后 6 PASS / 0 FAIL，exit 0**。旧代码三个失败断言分别是替换后结果、深层导入后结果、祖先导入的候选个数/边界断言。
- `git diff --check -- src/index/content_instant_session.cpp src/bench/content_instant_test.cpp` 成功。没有运行完整 content suite 或 UI/selftest。

## 验证边界

回归使用真实隔离目录和真实内容 scanner，但通知由 hooks 注入，且使用已知 FeedGap 对 pending 做确定性封口；它验证两个局部协调分支，并不声称端到端真实 watch/feed 通知时序已通过。没有动态制造访问拒绝或不完整枚举；保留错误行的结论来自明确的错误码分支检查。未检查 UI 主题/DPI/布局，也未对数据库吞吐、全量性能或外部 SMB/provider 行为作推断。
