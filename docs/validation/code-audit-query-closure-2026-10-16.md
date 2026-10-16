# 查询契约修复与定向验证（2026-10-16）

本次对应 outstanding 的 U07、U08 和 R08 中查询数值/日期输入部分。保留工作区已有的 QueryCanNarrow、type/kind 否定及其他修改。R08 的机器配置与 SQLite fallback 由其他工作包验收，不能由此报告整体关闭。

## 修复契约和静态依据

- U07：`!file:foo` 定义为 `!(is_file && name_contains_foo)`，`!folder:foo` 定义为 `!(is_dir && name_contains_foo)`。因此异类对象也命中；裸 `!file:`、`!folder:` 保持切换类型，双重否定还原正向语义。`MatchNameAndType` 在组合类型和名称后统一取反。文件 Engine 的常规/查询 shard 消费者、网络记录、live filename 和内容搜索 filename 谓词使用同一契约。
- U08：`MatchPath` 统一大小写、正反斜杠与 extended/extended UNC 前缀。无通配符的绝对条件按目录子树边界匹配，`C:\skip` 不命中 `C:\skip2`；相对跨组件条件对完整规范化路径作包含匹配；无分隔符条件继续逐组件匹配。通配符仍作用于完整路径；extended 前缀中的问号不当作通配符。首个正向绝对路径继续由既有 `path_prefix` 承载，第二个正向绝对条件与反向绝对条件由共享路径谓词执行。
- R08 查询输入：`ParseU64` 在每次十进制累乘前检查边界，单位换算在乘法前检查上限；最大 uint64 字节值仍合法。非法大小、未知单位、无效日期和逆序日期范围编译成显式 `Invalid` 谓词，恒不匹配；`!` 不将非法输入翻成全匹配。同一 AND 组被拒绝，OR 的其他合法组照常匹配。不是抛异常或报告 UI 错误的接口。
- 日期按公历检查年、月和日（含闰年、世纪），输入格式仍限四位年份 1601–9999。局部时间转换失败、零边界、非递增边界拒绝，避免退化成开放范围。保留现有本地日历范围和排他上界；没有改变系统时区或配置。
- 内容搜索 `MatchContentFilename` 原先忽略名称、扩展名、大小和日期的否定；本轮同步统一各谓词的否定，避免相同查询在内容搜索入口中出现另一套语义。

主要实现位于 `src/index/index_query.cpp`（数值/日期解析及共享匹配函数）、`src/index/index_query.h`（Invalid 状态）、`src/index/live_filename_search.h` 和上述实际消费者。网络测试包装仅在 `PULSE_NETWORK_INDEX_TESTING` 下暴露实际 `MatchNetworkRecord`，没有另写一套测试匹配器。

## 新增定向用例和运行边界

- `pulse_index_test.exe --query-boundaries`：使用四对象命中位集，同时调用真实 live 与 network record matcher。覆盖带值/裸类型/双重否定/中文别名/通配名称，skip/skip2，跨组件路径、第二个绝对条件、UNC 与 extended 路径，十进制溢出、单位乘法溢出、最大值、非法公历日期、逆序范围及 OR 隔离。
- `pulse_content_search_test.exe --filename-query-boundaries`：17 个纯内存 filename 谓词用例，覆盖上述内容搜索契约和扩展名/大小/日期独立否定，无用户文件或配置写入。
- 既有 `pulse_index_test.exe --folder-kind-negation` 可确认本次整体谓词调整保留 type/kind 目录否定。
- 文件 Engine 的实际 hit 集由主代理在其拥有的 Engine 测试文件中增加 selector；构建、Engine selector 和本页新增 selector 的执行结果由主代理串行汇总。

本工作包按分工未自行启动构建或测试进程。以下实际结果由主代理串行执行后提供，非历史测试结果：

| 检查 | 实际结果 |
| --- | --- |
| `pulse_index_test` 重编 | 退出 0，日志 `build-query-storage-final.log` |
| `pulse_index_test.exe --query-boundaries` | 53 PASS / 0 FAIL / 53 checks，退出 0 |
| `pulse_index_test.exe --folder-kind-negation` | 32 PASS / 0 FAIL，退出 0 |
| `pulse_content_search_test.exe --filename-query-boundaries` | 17 PASS / 0 FAIL，主代理前轮运行通过 |
| `pulse_index_engine_test` 重编 | 退出 0，日志 `build-engine-query-final.log` |
| `pulse_index_engine_test.exe --outstanding-query-only` | 最终 20 PASS / 0 FAIL，退出 0，日志 `pulse_index_engine_test-outstanding-query-final.log` |

Engine 初次 3 FAIL 集中在 `!file:foo`、`!folder:foo`、`!file:`。夹具通过 AddForTest 自动建立的 `C:\fixture` 目录本身也在该 scope 内；诊断确认三个实际集合均只比初版 expected 多该 scope 目录。主代理按现有作用域包含自身的契约修正测试预期，并新增空查询明确断言 scope 自身和四个叶对象。此轮只修正夹具预期，生产实现未变；最终独立 selector 为 20 PASS / 0 FAIL，退出 0。保留首次失败记录以区分夹具修正与生产缺陷修复。

本次另修复 `index_test_main.cpp` 的诊断输出：原 `wcout` 在中文标签后中断可见输出，改为 UTF-8 窄字符即时输出，并为 query selector 增加准确汇总。首次仅见 13 条输出的退出 0 结果不作为 53 条可见断言证据，以上表格采用诊断修复后的完整结果。未修改断言以消除该输出问题。

相关源码 `git diff --check` 退出 0，仅 live header 的 LF/CRLF 提示。Engine 最终独立 selector 已补齐实际命中集验证。未执行真实 SMB、真实磁盘索引或 UI 测试；这些定向谓词和 Engine 夹具验证不等同于端到端后端或 UI 验收。
