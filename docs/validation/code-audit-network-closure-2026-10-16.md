# U09 网络根配置修复记录（2026-10-16）

范围：NET-4 / U09；保留文件中此前的扫描/规范化改动，并按查询代理要求接入共享名称/类型及路径匹配 helper。

## 实现

- 删除局部宽松字符串数组扫描，复用 `pulse::json::Document` 校验完整 JSON 对象、唯一顶层 `roots` 数组和全部字符串项；拒绝无效 UNC、内嵌 NUL、非字符串项及重复 roots 键，不发布部分结果。未知顶层字段仍允许。
- `NetworkConfigLoadStatus` 区分缺文件、合法空数组、已加载、缺 roots 键、损坏输入及 I/O 错误。UTF-8 解码或 JSON 失败均返回 false 和非空错误；合法 JSON Unicode 转义及 surrogate pair 正确解码。
- `Start` 使用 `LoadConfiguration` 保留失败原因，`ConfigurationError()` 可读取；后续 AddRoot / RemoveRoot 回传该错误并保持内存及磁盘原样。修复原文件后需要重新启动/加载才能清除启动失败状态。
- `SaveNetworkRootsFile` 在原子替换前完整读取、验证现有配置；文件损坏/缺键/I/O 失败时拒绝保存，保护直接文件保存接口和配置路径切换后的错误文件。缺文件允许首次创建。

## 定向验证

测试入口：`pulse_index_test.exe --network-config`，只使用 executable 所在 build 目录的上级 `bench_data/pulse-network-config-<pid>-<tick>` 独立目录及 config_path hook；不调用 Start，不启动网络扫描或用户配置存储。通过 friend 调用 Start 使用的同一加载 helper 验证失败状态保留及 AddRoot/RemoveRoot 拒绝覆盖。映射盘解析也使用拒绝 stub，避免真实网络 API。

覆盖：缺文件、空列表、缺键、数字/错误类型、数组/对象截断、缺逗号、尾随垃圾、重复键、无效转义/Unicode、NUL、无效 UNC、无效 UTF-8；合法 Unicode UNC；失败后的直接保存/添加/移除保持原字节和内存；共享拒绝模拟 I/O；合法空配置后的添加和持久化 roundtrip。

主代理重建后执行 `pulse_index_test.exe --network-config`：60 PASS、0 FAIL、退出码 0。本代理已逐项读取日志：`docs/validation/audit-closure-2026-10-16/pulse_index_test-network-config.log`。静态差异检查（`git diff --check`）无错误。未验证真实 SMB、GUI 展示或并发外部编辑配置；本改动不提供跨进程配置文件锁。

主代理首次执行报告 `--network-config` 退出 `0xC0000409` 且无 stdout，根因仍未确定。随后增加 selector/fixture 最早 stderr 诊断和 flush、顶层 C++ 异常输出、逐 Check 的 UTF-8 narrow 输出及即时 flush；创建/清理目录使用 error_code 并输出错误，配置 hook 用 RAII 复位。夹具从 TEMP 改为上述仓库内 bench_data；所有用例保持，未删除或弱化断言。上述最终运行通过，但不能据此将首次退出归因为环境。
