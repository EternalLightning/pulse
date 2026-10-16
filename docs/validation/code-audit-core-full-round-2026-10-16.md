# Core 全文审查与定向修复（2026-10-16）

承接此前小批次，继续推进而未逐批请求确认。保留工作区所有既有未提交修改；没有提交、发布、安装更新或操作用户服务/配置。主代理复核具体修复路径，独立工作包由子代理审查，构建串行。

## 覆盖进度

本轮补齐 Core 清单此前剩余的 49 个文件：网络 8、变更跟踪 5、文档 4、查询/归并/拼音 5、内容搜索 7、内容索引 1、内容客户端 2、Shell/崩溃报告 5、资源 5、索引 host/client/MFT 5、引擎 2。`code-audit-core-coverage-2026-10-15.md` 共 157 项均已有全文阅读记录。

这只表示该清单的全文阅读完成，不表示全部缺陷已经修复，也不包含应用层/UI 尚未完成的阅读及动态验证。独立报告的局部依赖阅读不重复计数；测试源码中的断言不自动算作本轮通过。

## 本轮修复与验证台账

| 范围 | 改动 | 实际定向结果 |
| --- | --- | --- |
| 变更跟踪 | 同优先级采用最新事件分类，摘要与明细一致 | `pulse_change_tracking_test --merge-only`：修复前 2 FAIL/7 PASS；修复后 9/9 PASS |
| 网络索引 | 枚举失败保留旧分片；WNet 返回值；拒绝本地设备路径 | `pulse_index_test --network-scan-failures`：修复前 7 FAIL/4 PASS；修复后 11/11 PASS |
| 内容客户端 | 协调线程在释放查询所有权前 join，替换时先取消 | `PULSE_TEST_CLIENT_LIFECYCLE=1`：原逻辑子进程 terminate（91）、父测试失败；修复后 2/2 PASS |
| 查询缓存 | 复杂查询不再仅凭文本前缀复用旧候选集合 | `pulse_index_engine_test --query-narrowing-only`：修复前 6 FAIL/6 PASS；修复后 12/12 PASS |
| Shell 客户端 | 回收启动即退出的子进程状态；锁内复制错误消息 | `pulse_shell_launch_client_test --startup-recovery`：修复前 3 FAIL/3 PASS；修复后 6/6 PASS |
| 内容任务扫描 | 范围剪枝、空目录与异常枚举区分 | `pulse_content_search_test --task-enumeration`：保留 hooks 的原行为 8 FAIL/3 PASS；修复后 11/11 PASS |
| MFT | 映射未经验证时失败回退，禁止猜测连续布局 | `pulse_index_engine_test --mft-read-only`：修复前 7 FAIL/10 PASS；修复后 17/17 PASS |
| 文件修改通知 | 属性改变后使查询/文件夹大小缓存失效 | `pulse_index_engine_test --notify-modified-only`：修复前 16 FAIL/37 PASS；修复后 53/53 PASS（live 与 mmap 两条路径） |
| 中文资源 | 修正未配置排除规则时的矛盾说明 | 1055 个资源键/枚举/双语对应及占位符核对通过；47 个资源绑定资产存在。未做 GUI 验证 |

八组最终定向检查合计 **121 PASS / 0 FAIL**。计数包含夹具准备、结果与清理等断言，不表示 121 个独立产品场景，也不包含资源静态集合核对。每组都保留了修复前失败对照；内容扫描/网络使用枚举注入，客户端直接覆盖 retired 而 replacement 仅调用点复核。

## 后续仍需处理的发现

以下不能计为修复完成。按报告保留精确源码路径与复现建议：

- 引擎：跨卷 delta 回放的全局 ID 重排、目录移动后旧 DFS 范围、深路径截断、稀疏匹配集中的 overlay 重复候选，以及损坏 snapshot 的 parent 校验与重建配置并发访问。需要独立映射/多卷夹具，不能只靠编译验收。
- 查询：带名称的 `!file:`/`!folder:` 否定语义、跨组件路径谓词；本轮保留原行为，未混入缓存修复。
- 网络：损坏配置的解析与后续覆盖语义（NET-4）。
- 内容：传统扫描部分范围失败的成功状态、16 个长连接的 agent 容量处理、wire 的 root/roots 契约；共享索引属性读取失败后 prune 的候选风险。
- Host：重建命令忽略服务控制失败仍返回成功。
- 文档/资源：未确认其它可复现的新缺陷；真实 provider、渲染兼容性、语言切换和界面布局未动态验证。

## 验证边界

只运行相应 selector，未运行整套历史测试或完整 selftest。测试使用隔离文件、内存索引、枚举/I/O 注入或测试自身子进程；未连接真实 SMB、修改映射盘、ACL、全局配置或服务。沙箱构建多次停在 VerifyGlobs，停止本次拥有的进程后使用同一目标的批准提升执行；部分沙箱测试启动失败及默认 TEMP 下异常退出单独记录，不推断为产品回归。成功结果以实际运行的提升/隔离命令为准。

最终执行 `cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse pulse_index"`，退出 0；包括其既有依赖 Pulse.Shell 及资源编译，没有新增编译警告。该构建验证生产配置（不启用网络测试 hooks）。

本轮相关已跟踪文件的 `git diff --check` 通过；三个新增测试文件另行检查了行尾、尾随空白及 Unicode 替换字符。没有运行完整回归，也未启动新构建的 GUI/服务。编译成功不等同于服务、真实硬盘、Shell 扩展或 UI 功能验收。
