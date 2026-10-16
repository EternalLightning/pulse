# 小批次 IPC 审查记录（2026-10-16）

本阶段仅静态审查；未修改生产代码、测试或 CMake，未构建或运行测试，未提交、发布或清理现有修改。先阅读仓库 `AGENTS.md`，再检查目标文件工作区及暂存区 diff，六个目标文件均无差异。其他未提交文件保留原状。

## 结论

本批未确认新的、具备正常可达触发条件的功能或安全缺陷。下述现有防护是代码事实，不是本次新增修复，也不代表运行验证通过。分配异常覆盖范围保留为未证明的加固候选，不计作确认缺陷。

## 全文覆盖

| 文件 | 阅读范围 | 状态 |
| --- | --- | --- |
| `src/ipc/shell_interaction_client.cpp` | 1–97 | 全文 |
| `src/ipc/shell_interaction_client.h` | 1–15 | 全文 |
| `src/ipc/shell_launch_protocol.h` | 1–74 | 全文 |
| `src/ipc/shell_query_protocol.h` | 1–199 | 全文 |
| `src/ipc/shell_query_client.cpp` | 1–260 | 全文 |
| `src/ipc/shell_query_client.h` | 1–14 | 全文 |

共 659 行。未将下述必要依赖的定向阅读计入全文覆盖：

- `src/ipc/protocol.h:148–239`：字符串、数组的分配预算及解码失败行为。
- `src/ipc/shell_client.cpp:235–261,312–332,397–415,452–466`：请求登记、回调登记/移除、取消、响应类型和尾随数据验证。
- `src/shell_host/shell_query_worker.cpp:82–129`：父进程/管道对端鉴别、帧上限和请求解码。
- `src/shell_host/shell_launch_worker.cpp:70–77,296–304,328–336`：owner 归属、请求帧上限及响应封装。
- `src/shell_host/shell_interaction_worker.cpp:9–68`：选择器及提权等待返回值；仅为解释交互结果的定向读取。
- `src/app/app_commands.cpp:1720–1727`、`src/app/archive_dialog.cpp:151–154`、`src/ui/advanced_search_dialog.cpp:34–35`、`src/index/index_client.cpp:514–530`：交互调用点与返回值使用。
- `src/bench/shell_query_test.cpp` 的协议/生命周期断言及入口，`src/bench/shell_interaction_test.cpp` 的协议和交互测试范围，`CMakeLists.txt:512–522` 的对应目标；均未完整审查测试文件。
- 已有 `code-audit-*.md` 仅搜索本组符号，确认旧覆盖记录将这些文件标作未全文；未把其他审查记载的既有修复重新列为新问题。

## 已核对的边界

### 查询协议与结果类型

- `shell_query_protocol.h:87–111`：请求限制总帧大小、版本、kind 1–8、布尔字段、路径长度及 NUL、PIDL/marshal 各 64 KiB；两段字节之和必须精确覆盖剩余载荷。非 Icon 不得带属性，PidlName、InitializePreviewItem 和普通路径请求的字段组合相互分离；解析成功后才替换调用者输出。
- `shell_query_protocol.h:51–65`：PIDL 逐记录边界检查，必须以恰好一个末尾零 USHORT 结束；无终止符、短记录、越界及尾随字节均不通过。未声称能验证 Shell namespace 扩展内部的语义。
- `shell_query_protocol.h:148–192`：结果限制文本/PIDL/图标数量和像素尺寸，先验证字节数再复制；最多四幅、拒绝重复 list_id，最终由 `ValidShellIconPixels` 拒绝未定义的 list_id=3；stride 必须等于 width*4，像素长度使用 64 位乘法核对，最终 offset 必须等于 size。
- `shell_query_client.cpp:198–218`：回包 magic、type、request_id 和总长度必须匹配；进一步按请求类型限制 icons、pidl、filesystem_path 及 preview 初始化字段；成功的 PIDL 请求必须返回有效 PIDL，成功的普通文本请求不得返回空文本。不会把其他类型的可解析结果直接当作所请求类型使用。
- `QueryShell` 的 bool 表示 IPC/协议成功，Shell 操作失败通过 `result.error` 返回；不能仅因 bool=true 就认定操作成功。定向核对的调用者同时检查 HRESULT。

### 查询取消与资源生命周期

- `shell_query_client.cpp:79–99`：租槽等待持续检查取消和整次请求 deadline；取消回调抛出被转换为取消。池固定四槽，未取得槽时不生成第五个 helper。
- `shell_query_client.cpp:112–146`：新 helper 先以 suspended 创建并纳入 kill-on-close job，再启动；连接检查服务端 PID。已有进程失效或连接失败会进入 Retire。未对当前用户安全边界作跨仓库安全认证。
- `shell_query_client.cpp:149–183`：OVERLAPPED 和 event 的作用域结束前，在取消 pending I/O 后等待其完成，并恢复原始错误；未发现取消路径直接销毁仍被 I/O 引用的栈对象。
- `shell_query_client.cpp:102–109,240–253`：失败后只回收所租 helper；进程一秒内未确认退出则 poison，不立即复用槽；后续由 `ReapLocked` 确认退出再释放。Retire 最多额外等一秒，因此 timeout 是请求阶段 deadline，并非严格等于整个函数返回时间；现有测试也容许回收尾部时间。
- `shell_query_client.cpp:237–245`：只读请求最多一次重试；InitializePreviewItem 不重试，避免丢失响应后重复初始化同一 COM 对象。取消、超时和无效协议不重试。
- `shell_query_client.cpp:59–75`：池故意保持至进程退出，避免后台消费者晚于窗口 teardown 导致悬垂；job handle 的内核关闭承担进程退出回收。未将该有说明的进程级生命周期当作新泄漏。

### 交互与启动协议

- `shell_launch_protocol.h:41–71`：版本、kind、flags、NUL、过滤器成对/非空以及 picker/command/reveal 字段组合均检查，失败不发布部分 request。这里没有单独的 `size > kMaxPayload` 检查，但真实 worker 在 `shell_launch_worker.cpp:296` 先限制帧长；没有据此声称真实入口接受任意超大帧。
- `shell_interaction_client.cpp:7–18,43–57`：交互状态由后台线程和响应回调共同持有；完成只写一次，通过事件唤醒；局部函数提前取消返回时回调不会引用已销毁的栈状态。
- `shell_interaction_client.cpp:49–52,66–79`：取消发生在 request id 发布前由 cancel 原子位保留，发布后补发；id 已发布时立即发送；WM_QUIT 重新投递。读取 ShellClient 对应实现后，未将该发布窗口误判为丢取消。
- `shell_interaction_client.cpp:21–41,58–79`：仅在 owner 属于当前线程时禁用 owner 并泵消息；退出由 RAII 恢复原先可用 owner，owner 销毁后不再 EnableWindow。此处是模态等待，未承诺普通 query 的四秒超时，用户选择器等待不能仅凭无 deadline 判为新挂死。
- `shell_interaction_client.cpp:91–95`：PickShellPath 对失败、取消、空值、超过 32768 字符和嵌入 NUL 的响应不写回 path。

## 未证明的加固候选（不是确认缺陷）

### A. 查询前置分配在异常转换范围之外

- 精确位置：`shell_query_client.cpp:227–230`，catch 位于 `248–253`；相似的初始共享状态分配在 `shell_interaction_client.cpp:37`，其线程创建 catch 从 `42` 开始。
- 已证明事实：WriteShellQuery 的 vector 分配、首次 Pool 的 new/字符串分配若抛出异常，不经过 QueryShell 后面的 HRESULT 转换。解码分配有各自 catch，Exchange 分配位于 try 内；不能把这些已覆盖路径混为未捕获。
- 触发条件：必须在上述前置分配点出现 `std::bad_alloc`/相应分配异常，例如故障注入；本次没有证明普通文件名、有效 PIDL 或实际生产请求会稳定到达此失败，也没有声称造成已观察的用户崩溃。
- 最小复现方案：仅在后续决定修复时，用受控分配故障注入令合法短查询的首次序列化或 Pool 初始化分配失败，核对是否异常逸出；应在隔离程序中定位分配点，不要求消耗系统内存。
- 最小加固方案：将初始序列化和池取得纳入统一异常转换边界；如认为 API 应明确允许异常，则先补明契约，不机械扩大 catch。尚未修改。
- 验证边界：未运行故障注入，未检查全部上层异常处理；不计入缺陷数量。

## 如后续修复所需的最小验证

本批没有确认缺陷，因此未创建测试、未构建，也不因存在现有测试就默认运行整套。

- 若只改 picker/launch 解码，可选 `pulse_shell_interaction_test --protocol`，它已有奇数过滤器、NUL、字段混用及尾随字节用例；不需要自动启动真实对话框。
- 若改查询 namespace/PIDL，可构建对应 query fixture/test，再使用现有 `--namespace` 入口；若只改 preview 初始化，可使用 `--preview-item`。命令首参为 fixture 可执行文件路径，详见 `shell_query_test.cpp:506–525`。
- 当前 `LifecycleTests()` 没有独立 CLI 筛选入口；若后续修复取消/槽回收，应优先提供或采用最小定向执行方式，不因打包原因直接运行全部 query 测试。
- 若改模态 owner/WM_QUIT 生命周期，应增加或定向执行对应 owner 销毁、quit、取消先于 id 发布场景，核对 owner 状态、最终结果和 helper 回收；现有完整 picker 测试覆盖真实接受/取消/崩溃恢复，但本次未运行。

未验证范围：未运行实际 Shell provider、UNC、长路径、COM marshal、进程退出、资源计数或真实 UI 自动化；未进行恶意 helper 注入、分配故障注入或性能测试。静态阅读不能替代这些功能验证。
