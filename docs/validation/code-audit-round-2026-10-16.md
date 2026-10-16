# 代码审查增量汇总 — 2026-10-16

同日下一步已继续索引恢复和加载：见 `code-audit-shard-io-rebuild-2026-10-16.md`、`code-audit-cache-loading-2026-10-16.md`。以下 56 项结果仍为前一阶段记录，不与后续测试混算。

继续既有审查，保留全部已有未提交工作。此记录只汇总本轮实际改动和验证，不代表通读全仓或验收完成。

## 修复

1. 索引分片恢复：不再重新接受已因 CRC 被拒绝的 active；回退必须完整读取并成功持久化清单才返回成功。发布调用方遇到未解析的已有清单时，在移动候选文件前停止，避免默认覆盖 A 槽。详情：[分片恢复](code-audit-shard-recovery-2026-10-16.md)。
2. Shell verb 与重复扫描卷列表：结果交由窗口会话邮箱持有，关闭与发布同步；窗口消息不再承担堆载荷所有权，旧会话消息不能消费新结果。详情：[异步结果生命周期](code-audit-app-async-2026-10-16.md)。
3. 拖放 HDROP：GetData 成功后立即建立作用域释放，空句柄及错误介质类型拒绝时也释放 owner；读取前核对 TYMED_HGLOBAL。详情：[拖放补读](code-audit-drag-drop-2026-10-16.md)。

## 最终定向验证

| 命令 | 最终结果 |
| --- | --- |
| `pulse_index_engine_test --shard-recovery-only` | 15 PASS，退出 0 |
| `pulse_index_engine_test --shard-read-failure-only` | 13 PASS，退出 0 |
| `pulse_window_result_mailbox_test` | 11 PASS，退出 0，含 200 次发布/关闭竞争 |
| `pulse_drag_drop_test --medium-only` | 9 PASS，退出 0 |
| `pulse_drag_drop_test --target-session-only` | 8 PASS，退出 0 |

以上共 56 个 PASS，不重复计入开发中间轮次。分片和拖放均保留修复前失败证据；邮箱的旧路径风险由源码所有权关系确认，未先运行旧版进程级泄漏检测。各自测试边界见对应记录。

整合执行 `cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse"`，退出 0；受 app_state 头文件及 index_shard 修改影响，共计划 39 步，最终链接 Pulse.Index.exe、Pulse.exe。输出没有新增编译警告。没有运行全量构建脚本或完整 selftest。相关 tracked diff 的 `git diff --check` 退出 0，仅换行转换提示；新增邮箱和测试文件未发现行尾空白。

## 尚未完成

- 已有分片清单及两槽均不可恢复时，现在保守拒绝发布；自动废弃清单并重建的策略未实现。I/O 与损坏的 bool 状态混合、previous 缺少历史 hash 等边界也仍在分片记录中。
- 邮箱测试使用真实消息窗口；HWND 数值复用通过同一窗口上的新邮箱确定性模拟，未完成真实菜单/卷枚举 GUI、创建失败注入或第三方 Shell 阻塞验收。
- 本轮新增全文覆盖 drag_drop.cpp/.h、user_storage.cpp/.h、diagnostics_exporter.cpp/.h。配置迁移失败回滚的对象身份和诊断枚举错误传播仍需独立复现；没有把静态风险标成已验证缺陷。
- 未测试真实 SMB/RaiDrive、主题/DPI 或完整 GUI。旧 UI 台账剩 18 个未全文文件，其他模块也仍有覆盖缺口。全项目审查持续未完成。
