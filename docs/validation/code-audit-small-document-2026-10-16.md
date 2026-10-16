# 小批次代码审查：文档提取链路（2026-10-16）

## 范围与结论

完整逐行审查指定 7 个文件，共 507 行；先读取仓库 AGENTS.md，再检查这些文件的当前 diff（为空）。工作区存在大量已有修改，本阶段仅新增本报告，未改生产代码、测试或 CMake，未构建、未启动测试程序、未提交或发布。

本轮未确认可按缺陷提交的高/中优先级问题。有一个与补充平面字符有关的编码候选需要实际 PDF 样例复现；不能仅凭 API 注释断定随仓 DLL 的运行表现。以下结论是静态审查结果，不代表功能测试通过。

## 逐文件覆盖

| 文件 | 覆盖 | 结果 |
| --- | --- | --- |
| `src/index/document_filter.cpp` | 1–93 全文 | 检查注册表缓冲区、COM 对象、模块生命周期、IFilter 状态、chunk/text 循环、输出上限与 early-stop。无确认缺陷。 |
| `src/index/document_filter.h` | 1–9 全文 | 参数类型及默认 early-stop 参数与实现一致。 |
| `src/index/document_host_main.cpp` | 1–155 全文 | 检查请求长度、路径 NUL、配置/override 组合、文件属性和大小、begin/response/completion 状态、异常、PDF fallback、COM 线程与句柄释放。无确认缺陷。 |
| `src/index/document_protocol.h` | 1–56 全文 | 检查边界常量、结构默认值、固定宽度字段与已有布局断言；包含的 metrics 头提供 Windows 类型。无确认缺陷。 |
| `src/index/document_literal_match.h` | 1–34 全文 | 检查空 needle、跨块重叠起点、大小边界、大小写折叠表索引和调用条件。无确认缺陷。 |
| `src/index/pdfium_text.cpp` | 1–149 全文 | 检查 DLL 加载、符号缺失清理、Reader 边界/缓存、文档页文本 RAII、字符缓冲区/输出上限、CJK 拼接和终态。存在下述待复现编码候选。 |
| `src/index/pdfium_text.h` | 1–11 全文 | 串行隔离进程约束与当前调用方式一致。 |

## 已核对的防护与可达性

1. **输入长度与乘法。** host 106–114 行限制 path ≤32767、needle ≤4096、文件上限 ≤512 MiB，之后的 UTF-16 字节数乘法不溢出 DWORD；拒绝路径内部 NUL。needle 是带长度的视图，内部 NUL 不导致路径式截断。父调用者 `document_reader.cpp:159–178` 对过长 needle 禁用优化，请求载荷在 pipe 缓冲区范围内。
2. **IFilter 状态和资源。** `document_filter.cpp:68–91` 在非终止失败时返回错误；限制一百万 chunk，拒绝 count 越界、成功但无进展、超过 8 Mi 字符；末尾正常终止和命中 early-stop 才提交 body。ComPtr、stream 与 query.dll owner 均覆盖正常/异常退出。第三方提供程序若在调用内越界或挂起，函数级的返回后检查无法预防；已存在隔离进程、作业内存限制和父进程终止边界，未据此报告可复现漏洞。
3. **输出长度。** `pdfium_text.cpp:139–144` 为 CountChars+1 分配空间，符合随仓 `fpdf_text.h:378–384` 的缓冲区约定；追加页分隔符占用的一字符由前置 +1 预算覆盖。IFilter 每次 append 前也检查余量。host 138–142 行再次验证上限，并在 error 时清空正文和 file_bytes。
4. **增量匹配。** `document_literal_match.h:18–25` 从上次末尾向前回看 needle.size()-1 字符，覆盖跨 GetText/page 的命中。定向核对 `content_task_search.cpp:203–223,389–401`：仅在无排除词、非 whole-word、单个 literal 或完整 phrase 时传递 early-stop hint，不会因只读到一个 AND 条件就停止。匹配折叠方式与 `content_search.cpp:51–59` 相同。
5. **取消与释放。** 本组提取函数没有自身 cancellation 参数，这是现有隔离设计；定向核对 `document_reader.cpp:355–390`，父进程每轮接收检查取消/截止时间，失败路径 Stop 关闭带 KILL_ON_JOB_CLOSE 的 job（308–313 行）。不能把 provider 内部挂起误报为完全不可取消。PDF 的 document/page/text 采用嵌套 RAII；Reader 在 document 关闭前保持存活。PDF IFilter 线程 join 后再读取其结果，无并发读取 body/hr。
6. **错误和空 PDF。** `document_host_main.cpp:61–85` 将正常非匹配/纯图像与兼容性失败分开；只有指定错误触发兼容 fallback，空白成功提取最终变为 unsupported。错误正文不会通过 Response 暴露成成功。首轮 begin 已发送后的提取错误由 response.error 承载，父端不要求 begin.error 为后续失败预知结果。

## 待复现候选：PDF 补充平面文字可能被 UCS-2 API 丢弃

- **精确位置：** `src/index/pdfium_text.cpp:140–144`；契约依据为随仓 `third_party/pdfium/include/fpdf_text.h:378–386`，其中 GetText 声明提取 UCS-2 并忽略无法以 UCS-2 表示的字符。
- **可达触发：** 正常 `.pdf` 进入 PDFium 引擎，文档包含有正确 ToUnicode 映射的 U+20000 等补充平面字符，用户搜索该字符。host 仅对成功但全空白文本改报 unsupported，不会为混合 ASCII + 此类字符的成功文本尝试 IFilter fallback（host 61–80 行）。
- **现有防护：** 输出缓冲区和进程边界可限制内存问题；`Codepoint`/`Cjk` 支持代理对，但不能恢复 API 已丢失的文本。IFilter 能否正确提取同一字形未验证。
- **最小复现方案（尚未执行）：** 制作单页、明确 ToUnicode 映射的隔离 PDF，文本分别为 `A + U+20000 + B` 和仅 U+20000。用随仓 DLL 比较 CountChars、逐字符 GetUnicode 与 GetText 的返回文本，再通过既有 document reader 执行 case-sensitive 字面查询；观察返回正文是否保留代理对、是否漏报命中。应同时使用 BMP 汉字作对照，排除 PDF 字体/映射制作错误。
- **判断边界：** 本轮只确认了本地 API 契约与调用路径；尚未证明具体 DLL 会丢字，因此不定性为已确认 bug，不宣称已复现，不建议直接据此修改生产代码。
- **若复现后建议：** 评估支持完整 Unicode 的官方提取方式或 GetUnicode 后显式编码 UTF-16，并保持现有空白/分页语义、8 Mi 字符上限、early-stop 与资源边界。先建立单页 BMP/补充平面对照样例，再做定向验证。

## 验证与限制

实际执行：全文与定向调用点阅读；检查本组文件 diff；阅读随仓 PDFium 公共头文件契约；检查现有父端 cancellation/job/response 防护。未运行构建、测试、动态 PDF/IFilter 示例、真实提供程序、UNC/cloud/长路径样例或 UI 交互。

未完整审查 document_reader.cpp、content_task_search.cpp、content_search.cpp、document_metrics.h、document_file_version.h；上述文件仅按依赖需要读指定符号/范围。未审查 Office OOXML 实现和整个第三方 DLL。未将未运行的复现方案、编译或静态审查记作功能通过。
