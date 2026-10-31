# 2026-10-30 第二批内部修复

继续基于第一批未提交修改工作；保留另一会话的侧栏动画修改。用户要求尽量在 20 分钟内完成，优先限定内部正确性和重复工作，不调整待审查的操作设计。

## 本批完成

| 项目 | 改动与边界 | 实际验证 |
| --- | --- | --- |
| AI01 设备列表过期结果 | 定时回调派发请求时分配代数；进入 worker 和持锁发布时都拒绝旧请求，Stop 后拒绝迟到结果 | 确定性阻塞 A、先发布 B、后释放 A；保留当前列表、revision、目录 ID、历史信息；覆盖停止及排队失效 |
| AI05 云操作重复刷新 | 复用 ParentPath 规范化，按现有 Windows 路径比较方式去重，按首次出现顺序刷新唯一父目录 | 1000 个同目录文件只产生一个刷新父目录；跨目录、大小写、斜杠、UNC、扩展路径和根目录边界 |
| AC02 快捷方式元数据 | 目录通知在后台取得整批文件属性后，复用完整枚举的 ResolveLinksInPlace，再发布增量事实 | 22 项通过：普通文件、新增、目标类型及路径变化、重命名、失效目标、失败批次和删除项 |
| 旧内容索引 gap 恢复 | 文件源仍 ready 时首次 gap 立即重建基线并唤醒 writer；连接失败、未就绪、连续 gap 保留原退避 | 100 轮真实创建/删除全部最终收敛；性能仍有未达标项，详见下文 |
| 共享设置测试 | 保留原语言、缩放、宽度、空/非空矩阵；通过真实 scroll 和布局重算将按钮滚入视口，再检查可见性及生产 HitTest | `--layout-only`：108 PASS / 0 FAIL；原 6 项窄窗命中失败消除，没有修改产品设置页 |

设备与云路径共 17 项回归通过；快捷方式共 22 项通过。两组回归已保存为 `src/bench/app_integrations_lifecycle_test.cpp`、`src/bench/snapshot_patch_link_test.cpp`，加入独立、默认不构建的 CMake 目标。模型测试的快捷方式解析直接依赖已补齐。

没有修改 `src/ops` 或 `src/fs`，未改复制、移动、删除、回收、撤销的执行规则、授权或确认。快捷方式解析继续在后台使用现有有界 Shell IPC；实际 Shell 解析和大量快捷方式通知的耗时没有在本轮实测。

## 删除调查

原来的 `realtime_search content` 使用 LegacyWriter + shared scope，普通主窗口默认使用 Instant。测试计时从 `DeleteFileW` 开始，到应用结果模型中条目消失为止，不是删除调用耗时，也不是最终屏幕呈现时间。

确认的一处延迟来源：文件索引合并持久化快照时更新 feed epoch、清空旧事件流。内容索引将这个 ready gap 与断线混用 250 ms 退避；部分删除因此通过重同步清理结果，而不是直接消费删除事件。本批仅修复这段额外等待，不改变源 epoch 或删除语义；反复 gap 仍限制重试频率。

| 运行 | 创建到结果更新 P50 / P95 / P99 | 删除到结果更新 P50 / P95 / P99 | DeleteFileW 调用 P50 / P95 / P99 |
| --- | --- | --- | --- |
| 第一批原始 100 轮 | 391 / 609 / 657 ms | 563 / 953 / 1250 ms | 当时未分段记录 |
| 本批修复前 10 轮，仅记录 | 391 / 485 / 485 ms | 546 / 734 / 734 ms | 179 / 548 / 548 us |
| 本批修复后 100 轮 | 421 / 562 / 641 ms | 437 / 844 / 1031 ms | 244 / 1513 / 40140 us |

修复后为 **7 PASS / 1 FAIL**，失败仍是删除结果更新 P95 ≤ 300 ms。100 对文件操作全部成功并收敛，配置与定位器隔离哨兵保持不变。不同运行存在负载差异，表格不构成严格性能增益基准；原阈值和 100 样本要求没有放宽。

修复后 trace 有 100 个 gap，98 个删除窗口包含 gap，每个删除窗口至多一个；没有专门记录退避分支，不能将次数当作该分支从未执行的证明。阶段 P95：删除返回到文件通知 0.33 ms，删除返回到 gap 171.01 ms，gap 到内容提交 728.76 ms，提交到结果模型可见 30.83 ms。剩余主要耗时在 writer 恢复/重同步；现有 trace 不足以将其进一步归因到扫描或状态持久化。初始 epoch=0 请求不走宿主长轮询，已排除该猜测。本轮没有未经证据调整持久化或文件操作策略。

新增 `content-instant` 定向测试，复用私有配置并分开记录 DeleteFileW 微秒耗时。其首次运行发现夹具过早开始初扫：仅连接成功和 revision > 0 不代表初始 feed 已就绪，查询返回 ERROR_RETRY。现已增加真实 feed ready 屏障，并要求初始查询 error=0，避免把失败的空结果算作成功。

补齐屏障后，Instant 初扫成功，新文件在 203 ms 出现；删除调用为 247 us，但该轮结果在 5 秒内未消失，**未通过**。订阅日志明确记录 `reason=feed_gap`、`error=1022`（ERROR_NOTIFY_ENUM_DIR）、`during_initial_scan=false`、`known_pending=0`、`dropped=0`；删除通知后紧跟快照 merge。`content_instant_session.cpp` 对 gap 封存生产者并结束订阅，当没有待处理路径时不会重新核对现有命中项，因此保留旧结果。它与初扫的 ERROR_RETRY 是两个不同边界；恢复问题另列为待修项，不以 LegacyWriter 的成功收敛推断默认 Instant 同样通过。本轮不仓促重写覆盖缺失和订阅恢复语义。

## 构建与可用性

- `cmake --build build-release --target pulse` 成功，无新增编译警告；构建了当前主程序及受影响 helper。收尾再次构建退出 0，Ninja 确认无待构建项。
- 两个新增 CMake 回归目标均编译、链接成功，复制到自有 Medium 完整性目录核对 SHA256 后运行：22/22、17/17 通过。
- 共享设置定向夹具实际运行 108/108 通过，保留原零滚动截图及隔离哨兵检查。
- 六个生产 EXE 和两个 DLL 的运行副本逐项核对 SHA256。实际 `Pulse.exe --test-instance --shot` 在含中文文件名目录的亮色场景、空目录的暗色场景均正常启动、枚举、渲染并退出 0；两张图片已查看。

附加构建 `pulse_entry_order_hold_test` 未通过：`app_model.cpp` 引用了 6 个云目录符号，但该目标缺失对应实现的链接。相关引用在 HEAD 源码中已存在；本批新增的 shortcut 解析符号没有缺失。保留该失败日志，不把它报告为回归通过，也未扩大到修补其他历史测试目标。生产构建和本批独立回归不受此阻塞。

本轮没有打包、安装、替换已安装版本、发布或新增提交。基本生产可用性已经验证；真实 MTP/云提供程序、完整交互、实际文件操作 UI、Instant 订阅恢复仍不能据此宣称通过。

## 证据

详细材料在忽略目录 `bench_data/audit-fixes-20261030-batch2/`：`batch-start.patch`、`app-integrations.md`、`snapshot-patch.md`、`shared-settings.md`、`deletion.md`、`production-build.log`、`build-regressions.log`、`shared-settings-layout.stdout.log`、`realtime-legacy-before.stdout.log`、`realtime-legacy-after.stdout.log`、`realtime-instant-ready.stdout.log`、`production-smoke.log`。烟测图片、时序和哈希位于 `usability-57651dea706247f194d1a5e79770afc2/`。
