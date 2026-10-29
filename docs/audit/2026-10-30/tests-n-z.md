# 测试与诊断审查：src/bench 的 n–z / 非字母文件

审查日期：2026-10-30。依据本次工作树中的实际文件；不把历史运行记录当成本次测试结果。此包仅写入审查产物，未修改产品或测试源码，未删除文件，未提交，未构建，未运行测试或 GUI。

## 覆盖范围

- 原始分区为 `src/bench` 中 basename 以 n–z（不区分大小写）或非字母开头的全部文件，共 **136 个**。
- 本审查包已完整逐段阅读 **119 个**文件；逐文件清单为同目录 `tests-n-z-coverage.json`。清单与当前 `rg --files src/bench` 的分区结果核对，无范围外条目。
- 其余 **17 个**文件由主审转交其他包，未在本包冒充已读：6 个操作测试交 `audit_ops_fs`；11 个 t–z 文件交 `audit_index`。详细名单见文末。
- 已完整阅读 `CMakeLists.txt` 的全部 **1474 行**；还核对了下述发现所需的配置存储、helper 启动代码和相关脚本。
- 额外完整阅读 `scripts/build_background_window_fit_test.ps1` 及其对应的 `src/bench/background_window_fit_test.cpp`，用于回应主审的删除候选核实；不计入 119 个分区文件。

## 发现

### TB01 / P1：内容搜索测试只隔离 LOCALAPPDATA，仍可能覆盖真实用户的内容索引配置

**位置：** `src/bench/shared_settings_test.cpp:73–76`；`src/bench/realtime_search_test.cpp:23`、`:55–57`。

**触发：** 在正常用户会话中运行 shared-settings 测试，或 realtime-search 的默认 / content 分支；夹具位于允许持久索引的本地固定磁盘，且对应索引 helper 能正常启动。

**证据链：** 两个测试修改 `LOCALAPPDATA` 后，均调用默认的 `contentSearch.Start(nullptr, 0)` 再 `Configure(config)`。默认声明 `src/index/content_search_client.h:22` 为 `persistent=true, read_only=false`，实际进入 LegacyWriter；`src/index/content_search_client.cpp:200–216` 创建带独立 token 的自有 helper，继承环境。它没有复用正在运行的用户 agent，但隔离也仅覆盖部分路径：

1. `src/index/content_config_storage.h:11–20` 的 SQLite 路径使用环境变量，确实落在私有 profile。
2. 相同文件 `:82–86` 对默认数据库的配置路径改走 `storage::ConfigurationRoot()`，最终是 `content-index.config`。
3. `src/common/user_storage.cpp:312–323` 的默认根使用 `SHGetKnownFolderPath(FOLDERID_LocalAppData)`，不是被测试替换的环境字符串；`:25–32` 还可能遵循真实 `HKCU\Software\Pulse` 的存储位置选择。
4. helper 的 `src/index/content_agent.cpp:222` 处理 REQ_CONFIG 并调用 `ContentIndex::Configure`，后者在 `src/index/content_index.cpp:1212` 调用 `WriteConfigFile`；`content_config_storage.h:102–110` 的写入口会覆盖该配置。两测试未为配置根和 locator 设置隔离，`s.isolatedTest` / `appPrefs.persist=false` 没有保护 helper 侧写入。

**影响：** 测试可将真实内容索引的根目录、共享范围或编码改为夹具设置；已运行的用户 agent 也可能观察到配置变化。私有 SQLite 和独立管道不能证明真实配置未被修改。

**建议：** 给测试及其创建的 helper 传入同一套显式的私有配置根、索引根和 locator 身份，或提供专用配置存储注入。仅在父测试进程调用 `OverrideDefaultRootForTesting()` 不足以隔离另一个进程。增加一次定向验证，确认测试前后真实配置文件和 locator 字节不变。修复前不要把这两个默认内容分支纳入常规自动回归。

**验证性质：** 静态调用链确认；本次没有运行会写真实配置的路径。

### TB02 / P2：可移动设备策略测试把开发机 C: / E: 硬件布局作为必过条件

**位置：** `src/bench/removable_index_policy_test.cpp:79–82`、`:93–98`；构建入口 `CMakeLists.txt:268–271`。

**触发：** E: 不存在、系统盘不是 C:、或者 E: 合法地属于 USB / 可移动设备的 Windows 主机。

**证据：** 枚举断言要求真实挂载点中必有字面量 `C:\`；接着无条件对 `{C:\, E:\}` 逐一要求 `AllowsPersistentIndex(facts)` 为真。后者与产品禁止持久索引可移动介质的策略存在合法冲突。

**影响：** 没有产品回归也会失败，主机配置会被误报为策略或枚举错误；阻碍在其他开发机和隔离环境复用该测试。

**建议：** 保留现有合成设备、挂载别名、实时扫描和取消覆盖；真实系统盘通过系统目录解析，项目盘从夹具路径解析。真实设备探测应按探测出的事实比较策略预期，或使用显式参数 / 独立设备模式，缺少设备时报告环境条件。不要删除整项策略测试。

**验证性质：** 静态确认，未探测或修改设备。

### TB03 / P2：保存搜索测试共用固定临时文件，存在交叉删除和崩溃诊断风险

**位置：** `src/bench/saved_search_test.cpp:19–22`、`:33–44`；构建入口 `CMakeLists.txt:287–290`。

**触发：** 两个测试进程使用相同 TEMP 并行运行，或一个进程在另一个进程保存后、加载前执行清理。

**证据：** 所有进程都使用 `%TEMP%\pulse-saved-search-test.json`，启动和结束时均无条件 `DeleteFileW`。保存 / 加载过程没有进程隔离。`:37` 的数量断言是非致命 Check，随后 `:38–41` 仍直接索引 `[0]` / `[1]`；前序失败可能变成越界崩溃。

**影响：** 进程互相删除夹具并产生随机失败，或读取另一进程生成的数据而掩盖本进程保存问题；加载失败时还可能丢失正常的失败汇总。

**建议：** 使用 PID + 唯一 token 的专属临时目录并确认创建所有权，RAII 清理；数量不符时停止依赖该结果的索引检查。保留 Unicode、模式、持久化和去重覆盖。

**验证性质：** 静态确认，未并发运行测试。

### TB04 / P2：阻塞解码期间析构测试没有验证已进入阻塞点，可假阳性

**位置：** `src/bench/window_material_test.cpp:214–225`；构建入口 `CMakeLists.txt:651–661`。

**触发：** 专为该断言创建的解码线程在 1 秒内没有领取请求，例如启动失败或调度延迟；析构先设置停止状态。

**证据：** `:217–219` 最多等待 `DecodeAttemptsForTesting()` 增加，但超时后不检查计数；`:225` 仅以析构耗时 `<300ms` 报告 “destructor is bounded during stalled decode”。产品 `src/ui/window_material.cpp:776–778` 在 attempts 增加之后才进入注入的 1500ms 延迟，因此计数未增加时没有证据证明发生过被测阻塞。阻塞等待 worker 的析构回归也可能在未进入工作阶段时通过该断言。

**影响：** 该 PASS 不能始终证明注释和标签所宣称的析构边界。

**建议：** 保留用例，先对“确实进入该 generation 的延迟阶段”作独立必过断言，优先用 entered / release 事件门闩使阶段确定，再计时析构；未进入应明确失败或列为夹具初始化失败。

**验证性质：** 测试与 worker 实现的静态对照，未注入故障或计时运行。

### TB05 / P3：真实 exFAT 夹具每次重跑都重写 2026-10-23 的历史证据

**位置：** `src/bench/removable_exfat_device_test.cpp:185`、`:195–196`。

**触发：** 任何后续日期重新运行该设备夹具。

**证据与影响：** 输出 JSON 日期固定为 `2026-10-23`，并写回固定的 `docs/validation/issue-confirm-2026-10-23/exfat-device-validation.json`。该文件被 `docs/issue-bug-audit-2026-10-23.md` 及历史 finalize 脚本引用；重跑会把新设备 / 新源码的结果伪装成旧日期并覆盖旧证据。

**建议：** 默认按当前时间和 run token 写入新目录，记录源码 revision、设备 GUID 和实际运行时间；需要重写历史证据时显式指定输出路径。此源文件有真实 USB / exFAT 访问、策略拒绝及所有权校验价值，不能据此判为可删除诊断。未发现连接设备时返回 2 应按环境条件解读。

### TB06 / P3：播放 UI 截图输出忽略文件创建和写入失败

**位置：** `src/bench/playback_ui_test.cpp:179–185`，调用点为 `:397`、`:447`、`:450`、`:455`、`:466`。构建入口 `CMakeLists.txt:1288–1298`。

**触发：** `build\playback-check` 不存在、只读或磁盘写入失败，同时 GPU 渲染 / 回读成功。

**证据与影响：** `Capture()` 对 GPU target、EndDraw、回读和非空像素作了断言，但不检查 `CreateFileW` 的句柄，也不检查任何 `WriteFile` 返回值和写入字节数。调用者可以得到渲染 PASS / 进程成功，却没有预期的 BMP 视觉证据。该固定输出目录不由此测试的入口创建。

**建议：** 为输出目录显式创建并检查所有权，检查句柄、每次写入长度和最终文件，输出失败纳入 `failures`。这只影响截图交付可信度；文件内现有真实 GIF / 视频消息和异步提交断言仍有价值。

## 测试组织与保留结论

**确认可删除的测试源码或构建入口脚本：0。** 未因文件名带 probe / bench、没有字面量 CMake 文件名命中、截图需要人工检查或历史报告已有 PASS 而建议删除。

- `CMakeLists.txt:377–394` 保留默认关闭的 `PULSE_WITH_SELFTEST`；独立测试不会仅因为构建成功就运行。`PULSE_BUILD_ARCHITECTURE_FIXTURES` 在 `:1216` 默认关闭，较新的性能目标多为 `EXCLUDE_FROM_ALL`。部分其他测试默认属于 ALL 是构建组织问题，不能据此声称发行构建执行了测试。
- `CMakeLists.txt:813–825` 通过变量拼接登记 quick-access-badges、staging-tray、pane-filter、pane-animation 等模型夹具；只搜索完整文件名会误判为孤儿。其他文件可能由通用 `scripts/test_integration_fixture.ps1` 等入口编译。缺少 CTest 登记不等于测试不可用，项目使用自定义 exe / 脚本协议。
- `preview_handler_probe.cpp` 实际包含关联缓存、生命周期、资源上限、冷却及遮罩等回归，不是可直接删的简单探针。
- `pdf_bench.cpp` 的 JSON `ok:false` 和成功退出码是基准数据协议的一部分；`scripts/compare_pdf_engines.py`、`scripts/test_document_failure_metrics.py` 消费该结果，不能把退出 0 单独当作伪通过或无用文件的证据。
- `undo_identity_cases.h` 有有效的对象身份边界回归；`virtual_clipboard_test.cpp`、`shell_launch_client_test.cpp`、`shell_launch_test.cpp`、`shell_query_test.cpp` 和 `preview_handler_pan_test.cpp` 有取消、迟到结果、进程 / 管道及资源回收覆盖。保留。
- `operation_center_ui_test.cpp` 不仅截图，还检查任务排序、动作适用范围、窄宽 / DPI 边界、动画状态、更新期间按压、恢复资格复核及采样背景像素。生成快照不代表已完成人眼验收，但也不构成删除理由。
- `scripts/build_background_window_fit_test.ps1:5–10` 的确硬编码 `build` 并解析 Ninja 命令，维护性较弱；但 `CMakeLists.txt` 没有对应 target，该脚本仍是现有独立构建入口。对应源码覆盖 FitBackgroundWindowSize、原生设置动作、异步图像尺寸、宽高比、工作区和语言 / DPI 几何，未找到等价替代覆盖。应先迁移入口或增加 BuildDir 参数，不能先删除脚本。可重新生成的 obj / exe / rsp / cmd 与有效源码是不同的清理对象。
- 检查过的 Shell worker fixture 链接疑点不成立：相关运行分支受 `PULSE_OPERATION_ELEVATION_HOST` 宏保护；不报告为当前未解决链接缺陷。

## 实际验证和限制

完成的是全文静态审查、准确行号核对、分区清单比较、CMake / 脚本引用检查和有限的实现依赖跟踪。没有运行任何测试，不给出本次 PASS / FAIL 数量，不声称真实设备、COM provider、主题 / DPI、截图外观、安装版行为或性能已经验收。

建议先修 TB01 的跨进程存储隔离，再定向验证对应内容分支；TB02–TB04 只需围绕自身触发条件补强。相关检查通过后无需自动扩大到全部历史回归。

## 转交文件（不在本包 119 个已读清单中）

`audit_ops_fs`：

- `src/bench/ops_test_main.cpp`
- `src/bench/rename_ops_test.cpp`
- `src/bench/recycle_backend_test.cpp`
- `src/bench/operation_records_test.cpp`
- `src/bench/operation_elevation_adapter_test.cpp`
- `src/bench/ops_feedback_test.cpp`

`audit_index`：

- `src/bench/tab_indicator_animation_test.cpp`
- `src/bench/tab_indicator_ui_test.cpp`
- `src/bench/text_render_settings_test.cpp`
- `src/bench/text_task_reader_test.cpp`
- `src/bench/thumbnail_cache_test.cpp`
- `src/bench/titlebar_layout_test.cpp`
- `src/bench/transfer_pipeline_test.cpp`
- `src/bench/usn_packet_queue_test.cpp`
- `src/bench/window_transfer_regression_test.cpp`
- `src/bench/worker_lifecycle_test.cpp`
- `src/bench/worker_projection_cases.h`
