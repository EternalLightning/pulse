# 文档读取与 OOXML 全文审查（2026-10-16）

## 范围与结论

先读取 AGENTS.md、检查本组当前 diff（为空），并阅读 `code-audit-small-document-2026-10-16.md` 与覆盖清单中本组条目，避免重复已有审查。全文审查以下四个文件，共 929 行。本轮未确认需要修改的新缺陷；这是静态审查结论，不代表动态功能通过。仅新增本报告，保留已有工作区修改，未修改生产代码、测试或配置。

| 文件 | 实际阅读范围 | 核对内容 |
| --- | --- | --- |
| `src/index/document_reader.cpp` | 1–544 全文 | 扩展名路由、admission、进程启动/句柄继承、Job、请求/响应校验、超时取消、PDF OOM 重试、池 FIFO/租约/reaper、线程局部 session |
| `src/index/document_reader.h` | 1–34 全文 | 生命周期声明、不可复制会话、共享池、默认参数 |
| `src/index/office_ooxml.cpp` | 1–336 全文 | OPC 枚举、内容类型与 namespace、流预算、XmlLite 栈、共享字符串/单元格、文本追加上限、early-stop、失败清空及 COM RAII |
| `src/index/office_ooxml.h` | 1–15 全文 | COM 前置条件、失败输出契约和实现一致性 |

## 关键核对结果

1. **请求和协议范围。** reader 529 行在进入文档 worker 前限制路径为 32767 字符并拒绝内部 NUL；161 行将超过 4096 字符的 needle hint 清空。因此请求最多 73798 字节（72 + 2 × (32767 + 4096)），小于创建的 128 KiB 管道缓冲区，当前串行请求不会仅因合法最大输入超出缓冲区而阻塞写入。最大文件大小取调用者上限和 512 MiB 的较小值；响应正文限制 8 Mi UTF-16 code units，再按大小读取。Begin、Response、Completion 的 magic、flags、size/保留位与相关数值均有检查；文件版本校验只用于指定的新进程 IFilter 重试。
2. **取消与进程生命期。** worker 排队以 25 ms 检查取消，取得 worker 后建立 30 秒 deadline；该 deadline 贯穿 admission、启动后检查、两次读取尝试与接收循环。接收不在无数据时直接阻塞 ReadFile，先 Peek，再最多等待 10 ms 事件。失败/取消路径 Stop 关闭带 KILL_ON_JOB_CLOSE 的 job，等待最多 1 秒后关闭父端句柄。启动使用指定继承句柄清单及原子 Job 关联；启动失败各路径关闭已建资源。此结论不承诺 OS 调用本身或网络文件系统有绝对延迟上界。
3. **池和会话。** FIFO 队列持锁修改，取消/异常移除 waiter；Lease 在成功或异常返回时清除 busy 并通知。reaper 只 try-lock 空闲 worker，析构先 request_stop/notify/join，再销毁 workers。会话恢复 previous_pool，最外层退出才释放线程局部专属 worker；当前调用点均为栈上会话，未见跨线程销毁使用。
4. **重试边界。** 首轮只对 PDF、auto 模式、PDFium 引擎且确认为 OOM 的结果进入 fresh IFilter 路径；要求可用文件版本、配置未变，并沿用原 deadline。第二次 Begin 必须匹配版本/config/指定引擎；没有无限重试。正文仅在完整读取和 Completion 校验后提交，取消再清空输出和 bytes。
5. **OOXML 资源和预算。** OPC 输入上限为 512 MiB；相关 part 的声明大小及实际读取累计受 64 MiB 限制。最多枚举 65536 个 part、共享字符串最多 1048576 条且累计最多 8 Mi 字符；正文、单个 cell/item 的追加也受限。预算耗尽后额外探测一字节，以区分真正 EOF 与超额。XmlLite 禁止 DTD、限制深度 128；COM 对象由 ComPtr 持有，BudgetStream 的 remaining/metrics 引用寿命覆盖读取和释放。
6. **文本和失败提交。** strict/transitional namespace 均匹配完整后缀；Word/PPT 段落与各单元格有分隔符。共享字符串先读且仅被单元格引用时追加，忽略拼音注音；公式使用缓存值，字段指令不当作正文。共享索引检查十进制字符、乘加溢出及索引范围。抽取结果先写局部字符串，失败和异常保留空 output，成功才 swap。early-stop 是命中字面 hint 后允许的有意提前返回，不保证之后未读取 XML 的完整性校验。
7. **边界分工。** 非提取型扩展名直接调用 `text::ReadFile`，本文件只在进入前检查 cancellation，不声称文本解码过程具备该 worker 的隔离/超时语义。OOXML 本身没有 cancellation 参数，但生产 host 的调用位于上述父进程终止边界内；不能据此报告 provider 内部不可取消。

## 依赖阅读与限制

为核对调用契约，读取 `document_protocol.h`，`document_host_main.cpp:1–65`，`content_task_search.cpp:290–310,380–410`，`content_search.cpp:575–585,615–635`，以及现有 `src/bench/office_ooxml_test.cpp`。这些依赖不计入本轮新增四文件全文覆盖。旧报告中的 PDF 补充平面字符候选仍未复现，本轮没有将它升级为确认问题。

实际验证仅为全文/定向阅读、当前 diff 检查和现有测试源码核对。未构建、未运行测试、未启动真实 OPC/IFilter/PDF provider、未写用户配置。现有测试所列 DOCX/XLSX/PPTX、坏 XML、DTD、限额等场景只是阅读，未记作本轮通过。

未动态验证取消/进程崩溃竞争、内存分配异常、跨进程 admission 竞争、UNC/cloud/长路径、实际 Office 文件兼容性或 UI。OOXML 使用内容类型枚举而非完整 Office 渲染模型；关系所定义的展示顺序、Markup Compatibility 分支、图形/公式内文本和显示格式等语义未通过代表性样例验证，不作完整文档保真承诺，也不以这些未复现边界推定新缺陷。未完整审查 admission 实现、文本解码模块或 Windows OPC/XmlLite 内部。
