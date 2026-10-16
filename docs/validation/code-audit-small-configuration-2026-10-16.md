# 索引配置小批次审查（2026-10-16）

## 范围

主代理从当前 diff 和台账开始，全文阅读 src/index/index_config.cpp（420 行）、index_config.h（60 行）、content_configuration.cpp（69 行）、content_index.h（75 行），四文件开始时无差异。另全文复核已计入覆盖的 content_config_storage.h，不重复增加文件数。

Core 全文覆盖由 104 增至 108，未全文由 53 降至 49，总数仍为 157；此数字不代表全部应用/UI/操作模块。

## 已复现并修复：重复配置元素掩盖真正变更

原 SameConfig 先比较列表长度，再逐项检查是否能在另一个列表找到相同元素。该检查没有消费匹配项，因此旧 roots=[A,A] 与新 roots=[A,B] 被视为相等，且反向比较结果不同。excluded_directories 同样存在此问题。

WriteConfigFile 在读取到“相等”旧配置后直接返回成功，跳过写入。ContentSearchClient 的 Instant 配置控制经 GetConfig、SaveInstantContentConfig 到达该路径；这些校验未拒绝重复根目录/排除目录，因此不能假设输入一定唯一。结果是请求返回成功，但新根目录或新排除项没有持久化。SameConfig 还用于 ContentIndex 的外部配置刷新与变更判断。

最小修复仅位于 content_config_storage.h：用 std::is_permutation 比较 roots 和 excluded_directories，保留既有大小写无关路径谓词、root encoding 比较以及顺序无关行为，正确考虑重复次数。其余标量及 excluded_paths 的既有语义不变，不做额外规范化，不改变文件格式。

## 验证

既有 pulse_user_index_storage_test 增加独立 --config-equality 入口，直接使用真实 SameConfig、WriteConfigFile、ReadConfigFile。测试进程在任何存储调用前重定向 LOCALAPPDATA 并覆盖默认存储根，所有文件位于 bench_data/config-equality-<pid>-<tick>；fixture 保留检查，没有写真实用户配置或操作服务。

| 阶段 | 结果 |
| --- | --- |
| 原生产代码，新专项回归 | 4 PASS / 4 FAIL，退出 1 |
| 修复后相同专项回归 | 8 PASS / 0 FAIL，退出 0 |
| pulse_user_index_storage_test、pulse、pulse_index 构建 | 成功，退出 0，无新增编译警告 |
| 两个改动文件 git diff --check | 通过；仅既有 CRLF 提示 |

失败前后覆盖：双向重复根目录比较、真实保存后读回新增根目录、重复排除目录比较与保存、顺序/大小写等价、编码变化不等价。测试选择器在原主测试流程前返回，没有运行用户存储迁移历史整套。

命令：`cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_user_index_storage_test"`；修复后增加两个直接受影响生产目标；功能命令为 `build\pulse_user_index_storage_test.exe --config-equality`。使用此前可用的 require_escalated 执行环境；未运行全量 selftest 或发布构建。

## 其他检查与边界

- index_config.cpp 的目录创建/安全描述符、普通用户目录与机器目录分支、缺失配置默认值、旧配置位置兼容、原子写入、排除路径大小写和边界判断及卷枚举已读。ConfigureIndexPath 的现有调用均来自上轮加入预检的服务迁移流程，不将递归保护独立函数再重复报告为新的普通入口漏洞。
- content_configuration.cpp 的独立 sidecar 优先读取、旧 SQLite fallback、保存数量/大小/编码和根路径校验已读；content_index.h 的类型与公共接口已读。必要局部依赖包括 content_search_client.cpp 的 Instant 配置控制、content_search_protocol.h 的 GetConfig、content_index.cpp 的 SameConfig 调用，以及 index_path_service.cpp 的 ConfigureIndexPath 调用点；未将这些文件整体标作全文。
- 机器配置仍使用较宽松的本地数组提取器，旧 SQLite fallback 的损坏/失败行为也未做故障注入。本轮没有证明完整异常配置的恢复语义正确，后续应独立验证，不能由本次有效 sidecar 回归推断。
- 未运行真实设置 UI、后台索引重新配置、管理员机器配置写入、并发多进程或磁盘故障。保留所有既有未提交修改，未提交、发布或变更真实服务。
