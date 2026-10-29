# Shell / IPC 核对与修复记录（2026-10-25）

范围：`docs/report.md` 中 P-12/P-13/P-14，以及 P-23/P-26/P-27。删除计划、身份冻结及删除执行由其他工作包负责。本工作包未改删除代码、应用菜单/设置和 CMake，保留工作区已有修改。

## 逐项结论

| 项目 | 当前核对结论 | 本轮处理 |
|---|---|---|
| P-12 | 报告中 `Stop` 的无限线程等待确实存在；但“共享 bool 必然数据竞争”“handler 挂起永久耗尽四槽”的推导不成立，见下文 | 不改既有生命周期；保留定向进程测试用于验证 |
| P-13 | 多轮占位符替换、裸引号和 `CreateProcessW(nullptr, ...)` 的边界存在；终端目录参数已正确处理尾部反斜杠，不能归为仍有缺陷 | 单次扫描、统一 Windows 参数引号、显式可执行路径、拒绝歧义未引号路径 |
| P-14 | namespace / preview 初始化漏掉了已有 Shell 路径归一化；普通 UNC 或所有长路径必然失败尚无运行证据 | 两个入口复用 `StripExtendedPathPrefix`；补短扩展路径与超过 MAX_PATH 的真实查询/预览回归 |
| P-23 | 主管道名确实仅含 UI PID；`FIRST_PIPE_INSTANCE` 可被先占名阻断；客户端已核对实际服务端 PID | 保留问题；需要同步调整客户端、主机、子进程启动参数与测试协议，不在本次窄改动扩展 |
| P-26 | 通用 `RSP_DONE` 头部字段解析失败时，请求原先既不清理也不回调，代码证据成立 | 终态帧先取走 pending；所有解析失败都以 `E_INVALIDARG` 完成并清理 owner；重复帧不重复回调 |
| P-27 | `HostLog` 直接 `CreateDirectoryW` + `OPEN_ALWAYS`，未验证目录链或文件的 reparse/owner/ACL；代码事实成立 | 保留问题；影响条件取决于同用户预置路径、运行令牌等，本次未验证任意文件写入或提权 |

## P-12 的同步与回收证据

`src/shell_host/ctx_session_process.cpp` 中 `invoked` 仅由 `Core::Run` 写入。`Query` 不会重置它，而且已调用命令的 worker 本来就不能进入 idle。

`query_completed` 的 `Query` 重置发生在复用 idle 的交接中：`Run` 在 core mutex 下公布 `idle = session`（约 244 行），`Query` 在同一 core mutex 下预留它、设置 `next_id/next_query/query_completed` 并唤醒（约 279–303 行）；`Run` 唤醒后同样先取得 core mutex 再读取新会话（约 249–260 行）。因此该重置与后续 Run 读取具有锁建立的顺序，不能仅因读取位置没有 session mutex 就认定数据竞争。

`CtxWorkerHandles::Stop`（`ctx_handlers.cpp:509`）的 `INFINITE` 位于独立 context worker 内。外层 broker 的 `Core::Run` 在 close 时最多等待 500ms release ACK，随后不能复用就进入 `ProcessRetirements::Retire`。后者先终止进程，最多等 1000ms；若进程终止仍被驱动 I/O 挂住，保存句柄并继续占用 admission 槽，避免实际进程数超上限（`process_retirement.h:29`）。这种待退出进程仍可能占槽，是有意保持物理进程上限的错误边界，不能等同于普通第三方 handler 挂起永远不回收。

现有 `context_query_timeout_test.cpp` 的默认模式已覆盖初始 query hang、空 partial、可用 partial、终态超时和 close 后物理退出；`--reuse` 覆盖没有 cleanup ACK 的有界终止、invoked 不复用、四槽准入、idle 过期和 Stop 回收。应运行这些用例后再报告动态结论，不以静态分析替代执行结果。

## P-13 的行为边界

- 新增 `src/ops/shell_command.h`，以单次扫描展开 `%1/%L/%l/%V/%v/%*`；插入的路径不再参加后续替换。整体带引号的占位符和不带引号的占位符均使用 `QuoteWindowsArgument`。
- `OpsManager::OpenWithApp` 也使用同一引号函数；现有 `TerminalCommandLine` 已正确处理连续反斜杠与引号，本次不改。
- `shell_launch_worker.cpp` 先拆出 executable 和参数，向 `CreateProcessW` 显式传入 executable。740 提权回退复用相同解析结果，不再按前缀猜测另一程序。
- 未引号且首段含路径分隔符或驱动器冒号时，要求该首段已经以 `.exe/.com` 结尾；典型 `C:\Program Files\Vendor\app.exe ...` 返回 `E_INVALIDARG`。此类注册表命令需要修正为带引号的可执行路径。带引号的含空格路径、普通无空格 `.exe/.com` 路径、PATH 中的简单命令名和参数尾部均保留。
- 回归在隔离目录创建真实的含空格 EXE 与同名前缀 EXE，拦截 Win32 进程 API 验证歧义输入连 CreateProcess 都不会调用；另由已有子进程 fixture 验证正确命令真的启动并收到指定 CWD。

## P-14 的验证边界

`QueryShellNamespace` 和 `InitializePreviewItem` 现在与 `QueryShellName` 一样，在进入 Shell 解析 API 前移除 `\\?\` / `\\?\UNC\` 前缀，虚拟命名空间不受此 helper 改写。它统一调用约定，不会绕过 Shell provider 自身的长路径限制。

新测试要求短扩展路径产生相同 PIDL/文件系统名，要求真实超过 MAX_PATH 的目录查询和预览文件初始化成功。UNC 前缀转换覆盖纯路径语义；真实可访问 SMB 共享、快速访问 pin 动作和第三方预览 provider 未在本工作包实际执行，不能宣称通过。

## 定向验证入口

本工作包按协调要求未启动共享 build。以下由主代理在独立构建目录统一运行，并将真实结果补入主记录：

```powershell
.\pulse_shell_launch_regression_test.exe --commands-only
.\pulse_shell_launch_test.exe .\pulse_shell_launch_worker_fixture.exe --commands-only
.\pulse_shell_query_test.exe .\pulse_shell_query_fixture.exe --namespace
.\pulse_shell_query_test.exe .\pulse_shell_query_fixture.exe --preview-item
.\pulse_shell_launch_client_test.exe --malformed-results
.\pulse_context_query_timeout_test.exe --reuse
.\pulse_context_query_timeout_test.exe
```

已实际完成：相关 diff 人工核对；相关已跟踪文件的 `git diff --check` 退出 0（仅有仓库 CRLF 自动转换提示）。尚未在本工作包编译或运行上述程序，因此此文件不将功能测试标为通过。

## 后续管道夹具诊断

主代理已完成相关目标编译。`pulse_shell_launch_client_test --malformed-results` 能进入 main，但在 `PipePair::Connect` 失败，尚未进入 P-26 协议断言；同批其他部分程序存在 main 前 `0xC0000142`，两种现象须分开记录。

本工作包复跑该已构建程序得到相同的夹具连接失败；只给 `UserSecurity` / `PipePair::Connect` 补充失败阶段和 Win32 原始错误诊断，没有改变安全描述符、连接顺序或生产协议。特别是客户端打开失败时，在取消服务端 OVERLAPPED 前保存原始错误，避免清理时的 995 覆盖真实错误。

独立编译并运行 `shell_pipe_probe.cpp`（MSVC x64 `/W4 /MT`，不链接 user32，未启动共享构建），编译退出 0，无警告。探针在同一执行环境、同进程内重复夹具的管道创建/OVERLAPPED 连接/客户端打开：CreateNamedPipeW 成功；ConnectNamedPipe 为正常 `ERROR_IO_PENDING=997`；CreateFileW(client) 为 `ERROR_ACCESS_DENIED=5`。使用默认安全属性也相同。进程令牌 `integrity_rid=8192`、`restricted=1`。

据此，本次 P-26 结果应记为“受限执行环境下管道客户端访问被拒绝，断言未执行”，不能记作 P-26 行为失败或通过。没有发现夹具连接顺序错误的证据；未在非受限令牌下作对照，因此不推断具体限制策略。完整原始结果见 `shell-pipe-probe-result.txt`。新增夹具诊断源码仍待主代理重建该目标后确认输出。
