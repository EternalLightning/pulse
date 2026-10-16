# 索引日志、USN 与启动边界审查（2026-10-16）

子代理调用 502 失败后由主代理接管。全文阅读 src/index/change_journal_writer.h（47 行）、usn_stream.h（77 行）、index_service_start.h（43 行）、index_protocol.h（41 行），本组 diff 为空。未确认新缺陷；只读审查，未修改生产代码、运行构建或测试、操作真实服务。

- ChangeJournalWriter 使用 64 KiB 缓冲，批量写和直接写共享流状态；写失败后 Flush 返回失败。定向核对 ChangeTracker::Save：Flush 后 close，并且 written 与 out.good 都成功才 MoveFileEx 替换正式文件。未将未显式调用 ostream.flush 误报为忽略关闭失败。
- USN 读缓冲在 pending I/O 完成或取消排空后才复用/销毁；包入队成功才读取 cursor。UsnPacketQueue::Push 先检查至少包含 int64 cursor，再复制，不存在短返回立即 memcpy 的路径。析构先通知 stop、join 再关闭 stop event。
- EnsureServiceRunning 区分已运行、启动/停止过渡、停止后失败、启动后早退；新启动后再次检查存活。循环预算按等待次数计量，不是任意 SCM 系统调用的硬时限，未将其声明为严格 30 秒返回保证。
- Index header 复用公共 MsgHeader 默认初始化，设置 magic/type/id/size；host 在读取请求 payload 前限制 magic 与 256 KiB 上限。测试专用和存储停止消息由 host 状态限制，不能只凭枚举常量判断所有调用方都能停止服务。

必要局部依赖：change_tracking.cpp Save；usn_packet_queue.h Push/Take；usn_read_wait.h 的完成、取消、失败排空；index_host.cpp 请求头及 shutdown 分支；现有 change_tracking_memory_test.cpp 与 index_host_stress.cpp 仅符号定位。资源不足/线程构造异常边界缺少故障注入，未做推测性重构。没有验证实际卷读取、SCM 状态变化或远端阻塞。
