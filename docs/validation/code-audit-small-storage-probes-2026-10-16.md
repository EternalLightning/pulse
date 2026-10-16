# 用户索引迁移与维护诊断辅助审查（2026-10-16）

## 范围与结论

主代理先读取当前 AGENTS.md 和本组 diff（五文件无差异），全文阅读下列文件。本批未确认需修改的正常可达缺陷，仅新增本记录；保留已有未提交改动，未构建或运行测试。

| 文件 | 全文行数 |
| --- | ---: |
| src/index/user_index_storage.cpp | 76 |
| src/index/user_index_storage.h | 9 |
| src/index/index_memory_probe.h | 91 |
| src/index/index_maintenance_probe.h | 68 |
| src/index/index_diagnostics.h | 14 |
| src/index/index_paths.h | 14 |
| src/index/index_migration.h | 33 |
| src/index/search_trace.h | 29 |
| src/index/index_host.manifest | 25 |
| src/index/index_host.rc | 1 |

## 核对要点

- 用户索引迁移等待互斥量所有权，包含 abandoned 状态；写入者未退出时返回失败并保留原索引。定向核对 index_host.cpp 的 shutdown 分支及退出顺序：engine.Stop 完成后才释放互斥量，不能把收到停止请求当作写入已结束。
- host 自身启动时已持有同一个 mutex，Windows mutex 同线程递归获取允许该调用；helper 释放其增加的一次持有不释放 host 原持有。app_main.cpp 的迁移在创建窗口前的工作线程执行后 join，不在已运行 UI 消息循环中搬运文件。
- 复制失败不提交目标，提交失败丢弃本次副本，成功保留原 checkpoint 用于恢复。继续使用既有 CopyIndexForMigration 的 artifact 范围；没有将上一轮服务 ACL 修复重复记作本批修复。
- shutdown 请求的 OVERLAPPED、event 和 pipe 在 pending I/O 取消并等待结束后才释放，未确认栈对象过早释放。60 秒 mutex 等待不是整个迁移的端到端时限；复制时长及系统调用等待仍不受该时限约束。
- 内存采样使用固定数组和固定枚举，调用处均为已定义采样点；没有可达外部任意索引输入。采样失败增加 failures，不把旧样本更新为新 tick。维护原因来自固定字符串或默认 forced；未发现当前调用者传 null 给 strcmp。
- JSON 字段名固定、值为数字，原因字符串不直接拼入这些辅助 JSON。诊断开关只接受启动环境变量 PULSE_INDEX_DIAGNOSTICS=1，进程内静态缓存；不将运行中修改环境变量不生效误报为缺陷。
- 补读五个接口/资源文件：index_paths.h 与实现的冻结目录接口一致；index_migration.h 保留上轮已审修复，不重复计入新修复。search_trace.h 显式环境变量启用，CSV 路径双引号转义、换行替换，未启用时不写文件；调用 stage 是固定字符串。其同步 I/O、未轮转和异常传播属于显式诊断模式限制，本批没有端到端测试。host RC 确实引用 manifest；asInvoker/longPathAware 声明不等于运行时权限或长路径功能已验证。

## 必要依赖和限制

局部阅读：app_main.cpp 的启动迁移调用；index_host.cpp 的 singleton 获取、storage shutdown 和 engine.Stop/mutex 释放顺序；index_engine.cpp 的 MergeBase、maintenance reason 及工作循环；filename_timing.h 的 Memory/Maintenance 接口；current_user_security.h 的安全描述符构造；user_index_storage_test.cpp 的既有 checkpoint/host fixture。上述依赖不计为本批全文覆盖。

补读接口前检查对应 diff，仅 index_migration.h 有上一轮已知改动，原样保留。index_paths.cpp 仅作接口比对，其已存在的覆盖状态不变。本报告累计十个全文文件。

未执行真实 mutex/pipe/host 迁移、ACL、磁盘故障、诊断输出解析或并发测试。代码中部分裸句柄在分配异常传播时没有独立 RAII 清理，未证明正常输入触发，因此仅保留异常加固候选，不作推测性重构。进程级内存样本不能用作单一组件的独占内存或性能结论。
