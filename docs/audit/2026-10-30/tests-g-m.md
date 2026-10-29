# 测试与测量工具审计：g–m 分区

日期：2026-10-30。范围：`src/bench/` 下文件名以 g 至 m 开头的 **53 个文件全部通读**，包括头文件和资源文件。覆盖清单见 `tests-g-m-coverage.json`，路径相对于 `src/`。审查先核对负责范围的当前差异，再通读当前源文件；截断输出已补读。跨模块读取仅用于确认下列调用链，不计入 53 个文件。

本分区未修改产品源码，未构建或执行测试；仅写入本报告和覆盖清单。“确认”表示静态控制流、数据流或调用契约确认，未宣称运行复现。当前发现 1 项 P1、3 项 P2、1 项 P3，均可给出具体触发路径。

## TGM04 [P1] 全局搜索窗口测试会覆盖真实用户的内容搜索配置

- **位置**：`src/bench/global_search_window_test.cpp:138-141,369-374`。
- **触发**：默认运行该测试，在固定本地磁盘上的 fixture 中创建 needle.txt 后，执行 `state.contents.Configure(config)`。测试创建了 profile 并只修改 `LOCALAPPDATA`，全文件未调用 `storage::OverrideDefaultRootForTesting`。
- **调用链证据**：
  - `src/app/global_search_window.cpp:700` 以 `ContentAgentMode::Instant` 启动内容客户端。
  - `src/index/content_search_client.cpp:164-166,221-239` 将 Configure 放入命令队列，在 Instant 的 REQ_CONFIG 分支调用 `SaveInstantContentConfig`。
  - `src/index/content_configuration.cpp:139-141` 保存到 `WriteConfigFile(DatabasePath(), config)`。
  - `src/index/content_config_storage.h:11-20` 的数据库路径确实读取被重设的 LOCALAPPDATA；但 `82-86` 对默认数据库路径改用 `storage::ConfigurationRoot() + "\\content-index.config"`；`102-110` 以替换文件的方式保存配置。
  - `src/common/user_storage.cpp:312-323` 在没有测试 override 时使用 `SHGetKnownFolderPath(FOLDERID_LocalAppData)`，未从 LOCALAPPDATA 环境变量取真实配置根；`332` 还可能解析用户先前配置的自定义根。
- **影响**：数据库 profile 与配置目录没有一起隔离。正常写入成功时，测试会将真实内容搜索的 roots、排除和其他配置替换为 fixture 配置，既不备份也不恢复。已有真实配置还可使 `GetConfig().roots.empty()` 的等待条件提前结束，影响测试自身判定。本文没有运行测试去触碰真实用户配置。
- **建议**：在任何全局搜索窗口、内容客户端或 storage 首次访问前，对 profile 调用 `OverrideDefaultRootForTesting`；同时保留需要的数据库环境隔离，并验证所有持久文件均处于该 fixture。不要依靠事后恢复真实设置来替代事前隔离。为既有用户根使用只读哨兵或注入路径解析做定向验证。

## TGM01 [P2] 性能验收把读取失败当作成功样本

- **位置**：`src/bench/index_perf_main.cpp:49-64,67-104,161-179,192`。
- **触发**：请求成功写入索引管道后，服务在有效响应前关闭连接，或者返回坏 magic、超长或截断帧，导致 `ReadFrame` 返回 false。
- **证据**：QueryStatus 和 RunSearch 都先令 `ok = PipeWrite(...)`，再用 `while (ok && ReadFrame(...))` 读帧。ReadFrame 的 false 仅结束循环，不会把 ok 置为 false；没有“已经收到并解码匹配响应”的状态。两个函数最后均返回 ok。
- **影响**：调用方将未收到响应的请求计入时间统计。Sample 的 total、returned、error 默认为 0，incomplete 默认为 false（22-27），因而该样本仍满足 quality PASS；在耗时符合预算时，`--enforce` 也可能以 0 退出。质量字段解码虽比旧版本更严格，仍不能证明确实接收了响应。
- **建议**：以明确的匹配响应接收并完整解码为成功条件，所有读取失败和流结束均失败。定向回归应覆盖“写成功后 EOF”、坏头、截断载荷，以及无关响应后 EOF；断言失败退出且不输出有效 PASS 样本。无需运行真实全盘索引来验证这一控制流。

## TGM02 [P2] MFT 基准把续读游标头当作第一条 USN 记录

- **位置**：`src/bench/measure_mft.cpp:172-203`，尤其 `186-193`。
- **触发**：卷可打开且 `FSCTL_ENUM_USN_DATA` 成功返回数据。该基准从 `buffer.data()` 直接转换为 `USN_RECORD_V2*`，把整个 bytes_read 当作记录长度。
- **证据**：同一接口的生产调用在 `src/index/index_engine.cpp:3156-3164` 先要求至少 `sizeof(USN)`，将头部 8 字节取为下一次 StartFileReferenceNumber，再从 `buffer + sizeof(USN)` 校验并枚举记录。基准没有跳过该头部，反而把每条“记录”的 FileReferenceNumber 当作续读游标。
- **影响**：记录数和速率不是实际的完整 MFT 枚举结果。头部被误作 RecordLength 时可能立即退出内层而保留相同 StartFileReferenceNumber，外层重复读取同一页直到五分钟预算；也可能按伪记录推进并统计错误记录。不能据此接受文件规模、吞吐或内存估计。
- **建议**：复用生产 USN 记录验证/遍历规则：先检查返回长度，读取头部续读游标，从头部后遍历完整记录，并拒绝无进展游标或异常记录。用带已知页头和两条记录的合成缓冲区做定向校验即可，不必扫描用户卷。此处为接口和本仓生产解析的静态对照，未实测扫描。

## TGM03 [P2] Shell 基准在缩略图对象创建失败时读取未初始化耗时

- **位置**：`src/bench/measure_shell.cpp:93-109,124-145`。
- **触发**：采样列表取得路径后，`SHCreateItemFromParsingName` 失败，例如文件在枚举与采样之间被移除或对应 Shell 对象无法创建。
- **证据**：time_thumbnail 只初始化 ok=false；ms 仅在 `SUCCEEDED(hr)` 分支的 102 行赋值。调用方在 126 行声明 `double a, b, c;`，130 行传 c，133 行无条件将 c 加入 t3。
- **影响**：失败路径把未初始化 double 读入缩略图时间分布，属于未定义行为；总样本的 mean、median、p95、max 和 100k 外推值均不可信。hits-only 统计有 ok 保护，不抵消总统计的问题。
- **建议**：所有返回路径初始化耗时并明确其含义，或返回带成功状态的样本；总尝试耗时可计时包含对象创建阶段，失败样本则单列。对“创建对象失败”的分支做定向验证，确认没有未初始化采样且失败数量可见。

## TGM05 [P3] 编辑器稳定性失败截图可能未落盘，且日志路径与实际名称不符

- **位置**：`src/bench/hosted_edit_stability_test.cpp:69-88,327-331`。
- **触发**：测试在新的 checkout 中运行，`bench_data/edit-surface-20261024` 不存在，或截图输出失败；另外任何 failed_frame 分支都会触发路径名称差异。
- **证据**：SavePixels 用 ofstream 写固定子目录，全文件没有创建该目录，也不检查文件打开/写入结果，返回 void。failed_frame 中的 failure_name 已前缀 text_mode，SavePixels 又自动前缀 CurrentTextRenderMode 得到的 mode_name；日志仅输出未再加前缀的 failure_name。
- **影响**：出现需要分析的渲染失败时，可无声丢失关键图像；即使目录预先存在并成功写入，ARTIFACT 行仍可指向不存在的文件名。它不把实际像素断言改成通过，但破坏失败证据的可检索性。
- **建议**：先创建输出目录；由保存函数返回成功状态和最终实际路径，日志直接使用返回路径。失败时报告明确 I/O 错误，避免声称已经生成工件。此项不需要扩大为全量 UI 回归。

## 审查边界与未升级的观察

- 文件中的诊断模式和实测模式分别判断：例如 index_security_test 的 metadata budget/visibility 分支是报告指标的诊断入口，不能仅因没有通用退出阈值而判定“假通过”。
- locked_operation_test 的终止进程路径先校验锁持有 PID 均属于自建 Holder；多窗口打开探针还校验请求者、broker、请求号和 fixture 目标后才确认安全警告。未将这些受控操作泛化为“会结束用户进程”。
- 若干测试使用固定或 PID 命名目录，可能存在重入、并行或残留前提；未在没有具体失败链时全部列为缺陷。index_test_main 的固定 TEMP fixture 值得后续统一隔离，但本报告未据此推断已有用户数据损坏。
- 没有把 API 初始化失败后可能继续执行等通用脆弱性机械列为产品问题。没有宣称这些 53 个文件对应的测试已通过，也没有将静态源码覆盖等同于 UI、服务、真实设备或数据安全的运行验收。

