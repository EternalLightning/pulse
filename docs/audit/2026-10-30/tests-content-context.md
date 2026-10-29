# content/context 测试源码审查

日期：2026-10-30。范围：`src/bench` 中 basename 以 `content` 或 `context` 开头的 22 个文件。按主代理调整后的分工，本报告负责其中 **14 个新增文件，均已完整阅读**；另 8 个文件已经由 tests-a-m 分区覆盖。当前包的机器清单只登记新增 14 文件，避免重复计数。22 文件的并集已核对，无遗漏。

仅写本报告及覆盖清单；未修改源码、构建、运行测试、删除夹具或操作真实配置。下面均为静态审查结论，不能据此称相应用例已经运行通过或产品缺陷已复现。未确认 P0/P1 问题。

## TCC01 — P2：取消回归用例在超时后仍无条件 join，关键回归会挂住测试

- **位置**：[content_instant_test.cpp:221](E:/Projects/pulse/src/bench/content_instant_test.cpp:221)，入口 [content_index_test.cpp:1710](E:/Projects/pulse/src/bench/content_index_test.cpp:1710)。
- **触发条件**：以 `PULSE_TEST_INSTANT_GAPS` 运行时，通知等待出现本用例要捕获的回归，取消标志不能唤醒 `RunInstantContentSession`。
- **静态事实**：221 行把会话放入线程；223 行设置取消；224 行的 `Await` 只等待最多 2 秒；225 行随后无条件调用 `active.join()`。226 行记录失败发生在 join 之后。入口直接调用测试，没有子进程截止时间。
- **影响**：2 秒到期不能让测试输出该失败或退出。若会话永久阻塞，整个测试进程永久卡在 join；若很晚退出，用例也会超过其声称的边界。外部运行器可以强杀进程，但该用例自身没有完成边界，不能把 `Await(..., 2000)` 当作整个取消检查有界。
- **建议**：复用本文件已有 `RunClientLifecycleTest` 子进程方案（[content_index_test.cpp:101](E:/Projects/pulse/src/bench/content_index_test.cpp:101)），由父进程在 deadline 后记录退出失败并只终止其创建的子进程；不要 detach 持有本地引用的线程。
- **待验证**：让会话故意忽略取消，确认父运行器按截止时间失败退出且回收全部子进程；正常取消仍要求两秒内完成。本轮未注入回归、未运行。

## TCC02 — P2：旧会话调用断言接受成功回复，不能证明旧 token 被拒绝

- **位置**：[context_query_timeout_test.cpp:100](E:/Projects/pulse/src/bench/context_query_timeout_test.cpp:100)，支持链 [ctx_session_process.cpp:318](E:/Projects/pulse/src/shell_host/ctx_session_process.cpp:318)。
- **触发条件**：`--reuse` 测试已关闭 session 10，并把同一 worker 复用为 session 11；随后使用旧 ID 10 执行 Invoke。
- **静态事实**：回调在 70–72 行已保存回复中的 HRESULT；100–104 行却只检查 `invocation_results.contains(110)`，再检查 active/idle 数量，没有检查错误值。成功回复 `S_OK` 与当前正确的 `HRESULT_FROM_WIN32(ERROR_NOT_FOUND)` 都满足“收到回复”。夹具 worker 在 47–49 行对收到的 invoke 无条件返回 S_OK；后续代码没有重新检查 request 110 的结果值。生产拒绝分支在 333 行明确发送 ERROR_NOT_FOUND。
- **影响**：这个标为“stale close and invoke cannot address the reused process”的断言不能区分正确拒绝与错误转发给当前 worker。worker 数量仍为 active=1、idle=0 时，错误的成功调用不会被本断言识别。这里发现的是回归测试缺口，未发现生产代码当前已经错误接受旧 token。
- **建议**：断言 request 110 的确切拒绝 HRESULT；同时在夹具记录被调用的 session/次数，并用有效 session 11 再执行一次命令，验证旧 close/invoke 没有改变当前会话。
- **待验证**：通过可控坏实现或测试接缝让旧 token 获得 S_OK，确认修订后的用例会失败；现有正确实现应返回 ERROR_NOT_FOUND。本轮未运行。

## TCC03 — P2：固定 contenttest 目录使并行进程相互删除夹具

- **位置**：[content_search_test.cpp:25](E:/Projects/pulse/src/bench/content_search_test.cpp:25)、[content_search_test.cpp:128](E:/Projects/pulse/src/bench/content_search_test.cpp:128)、[content_search_test.cpp:505](E:/Projects/pulse/src/bench/content_search_test.cpp:505)。
- **触发条件**：同一个输出布局中同时启动两份 `pulse_content_search_test`（包含 `--fixture-only`）；相同 checkout 的同层单配置 build 目录也会映射到同一位置。
- **静态事实**：`FixtureRoot()` 恒定返回可执行文件 build 父目录下的 `bench_data/contenttest`，不带 PID/tick。每个进程在 128 行先 remove_all，再创建夹具；各自在 505 行再次 remove_all。420–424 行的独立 profile 也位于同一固定目录内，没有解决进程间隔离。
- **影响**：后一进程可删除前一进程正在扫描或供 IPC 使用的文件和 profile；一方提前完成也会删除另一方夹具，造成与产品逻辑无关的缺失、计数错误和不稳定结果。此处未发现删除仓库外用户文件的依据。
- **建议**：按其他 content 测试惯例使用 PID/tick 唯一目录，并让清理对象只拥有该进程新建的目录。保留 `--fixture-only` 选项的原行为范围。
- **待验证**：并发启动两份 fixture-only，各自目录不同、均能完成，并确认清理互不影响。本轮未启动测试。

## TCC04 — P3：失败夹具保留提示与无条件 RAII 清理冲突

- **位置**：[content_index_test.cpp:1699](E:/Projects/pulse/src/bench/content_index_test.cpp:1699)、[content_index_test.cpp:1933](E:/Projects/pulse/src/bench/content_index_test.cpp:1933)。
- **触发条件**：默认测试任一检查失败，走到末尾的 `Fixture retained` 提示并返回。
- **静态事实**：1934–1935 行只在成功时显式删除，失败时打印保留路径；但 1699–1702 行 `CleanupFixture` 的析构无条件 remove_all，同一个 return 会触发析构。正常可删除的夹具仍会被清理。早返回的子测试也受相同 RAII 清理支配。
- **影响**：日志声称保留的数据库、sidecar 和文件可能已消失，削弱失败分析，也使“失败保留现场”的执行结果不可信。删除失败时可能偶然留存，不能依赖它作为保留策略。
- **建议**：统一清理策略，用成功/失败状态控制 RAII；保留失败现场时也应打印确实存在的目录。避免末尾显式清理与析构表达两套相反规则。
- **待验证**：注入确定性失败并检查退出后的路径；成功运行则检查目录清理。本轮未运行。

## 已核对但未升级为缺陷

- `SavedSearchStore::Add` 只修改内存（[saved_search.cpp:111](E:/Projects/pulse/src/app/saved_search.cpp:111)），所以 `RestoreContentSearch` 中 Add 本身不能证明测试写入真实配置；未把它列为配置污染。
- legacy persistent client 与显式 Instant/Observer 用例属于不同契约。当前 `Start(HWND, UINT, bool, bool)` 仍明确选择 LegacyWriter，不能仅因应用已有 Instant 模式而判定旧持久化测试过时。
- context 菜单的 synthetic、renderer、真实 Shell、进程生命周期测试覆盖不同层次。共同的“late/stale/cache”主题不足以证明可安全合并或删除。
- `content_search_test` 默认含真实 `C:\\` 列表探测（387–418、472–500），而 `--fixture-only` 会跳过；这说明默认入口存在机器环境依赖，不把它等同于隔离夹具结果。文件没有写入 C 盘测试对象，但会读取真实目录元数据。
- `context_menu_settings_ui_test` 在构造 Fixture 前设置 storage 测试根（498 行），并设置相关 persist=false；系统页面使用合成展示数据，未据此报真实注册表修改。
- `content_index_test` 1937 行，许多分支由环境变量选择；`content_paging_test` 同样具有独立命令行分支。后续维护可补充定向入口清单，避免只跑默认入口却声称覆盖全部模式；本轮没有将缺少默认执行登记认定为死代码。

## 覆盖清单

本报告机器清单 `tests-content-context-coverage.json` 登记下列 **14/14** 新增文件；共 6135 行（按 Get-Content 统计，包含空行）。均按实际工具输出完整阅读：

1. `src/bench/content_index_test.cpp`
2. `src/bench/content_instant_test.cpp`
3. `src/bench/content_paging_test.cpp`
4. `src/bench/content_progress_ui_test.cpp`
5. `src/bench/content_reconcile_capacity_cases.h`
6. `src/bench/content_refresh_test.cpp`
7. `src/bench/content_reveal_ui_test.cpp`
8. `src/bench/content_search_test.cpp`
9. `src/bench/content_timing_test.cpp`
10. `src/bench/context_menu_loading_test.cpp`
11. `src/bench/context_menu_refresh_cases.h`
12. `src/bench/context_menu_settings_ui_test.cpp`
13. `src/bench/context_query_timeout_test.cpp`
14. `src/bench/context_session_process_test.cpp`

另 8 个文件的正式覆盖归属为 `tests-a-m-coverage.json`，不在本包重复计数：

- `src/bench/content_contract_cases.h`
- `src/bench/content_interaction_test.cpp`
- `src/bench/content_lifecycle_test.cpp`
- `src/bench/content_task_enumeration_cases.h`
- `src/bench/content_transient_error_cases.h`
- `src/bench/context_menu_overflow_cases.h`
- `src/bench/context_menu_reuse_probe.cpp`
- `src/bench/context_menu_sevenzip_cases.h`

前六项也已由本代理阅读，最后两项未重复读取。本报告引用其覆盖归属，不声称在本包新增完整阅读。两份机器清单的 content/context 路径并集为 22 个唯一文件。

