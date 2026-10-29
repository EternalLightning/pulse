# 公共层、模型、IPC 与宿主审计

日期：2026-10-30。范围：`src/common` 32、`src/ipc` 14、`src/model` 14、`src/preview_host` 13、`src/shell_host` 30、`src/integration_host` 1，共 **104 个文件全部通读**，包括资源、manifest 和头文件。对应清单见 `processes-common-coverage.json`，路径相对于 `src/`。先读取了仓库规则和负责目录的当前差异，再读取当前完整内容；截断输出已按范围补读。跨模块只针对实际调用链作定向读取，不计入这 104 个文件的覆盖。

本次只写审计材料，没有修改源码、删除文件、构建或运行测试。以下“确认”均指静态代码与调用链确认，不能替代运行验证。未发现足以定为 P1 的问题。

## PC01 [P2] 类型名称请求携带属性后，会被客户端的协议校验直接拒绝

- **位置**：`src/ipc/shell_query_protocol.h:100`；调用链 `src/ipc/shell_query_client.cpp:223-229`、`src/app/app_runtime.cpp:101-123`；接收端意图见 `src/shell_host/shell_query_metadata.cpp:35-50`。
- **触发**：详情元数据工作线程成功取得文件属性后，将 `kind=TypeName`、`attributes=fad.dwFileAttributes` 交给 `QueryShell`。普通文件的属性通常也非零，不只云占位文件会触发。
- **证据**：`QueryShell` 在取得进程池租约前，用 `ReadShellQuery` 自检序列化结果；该函数对所有非 Icon 请求执行 `if (decoded.kind != ShellQueryKind::Icon && (flags || decoded.attributes)) return false;`。接收端却专门为 TypeName 读取 `request.attributes`，据此选择不会访问云文件内容的 `SHGFI_USEFILEATTRIBUTES` 路径。
- **影响**：已有调用方与协议形成矛盾，正常详情请求得到 `E_INVALIDARG`，Shell 类型名称不会返回；新加入的占位文件保护分支也到不了。最终是否显示空值或其他后备文本，还取决于详情 UI 的后续显示逻辑，本文不把它描述为已观察到的 UI 现象。
- **建议**：按请求种类明确属性字段的合法性，允许 TypeName 的 attributes，仍拒绝该种类不支持的 `use_attributes`。同时校验发送端、客户端自检和宿主使用的是同一契约。
- **验证状态**：静态确定。尚未运行普通文件与云占位文件的详情展示验证；定向回归应覆盖 TypeName 的非零属性序列化往返以及 Icon 以外种类的非法字段。

## PC02 [P2] 搜索查询回填后会静默丢失自定义大小和日期约束

- **位置**：`src/model/search_query.cpp:278-290,315-317`；编译端 `src/model/search_query.cpp:187-193,202-211`；实际入口 `src/app/app_commands.cpp:1683-1689,1703,1749-1767`、`src/app/address_search.cpp:204-218`。
- **触发**：已有查询包含 `size:>42mb dm:2026-01-01..2026-06-30`，随后打开高级搜索并接受，或只改变类型筛选，或继续编辑搜索词。
- **证据**：`ParseSearchQuery` 只设置 `SizePreset::Custom` 和 `DatePreset::Custom`，未填充 `size_custom`、`date_from`、`date_to`。这些字符串默认均为空。`CompileSearchQuery` 在 Custom 分支仅从这些字符串输出约束，空值完全不输出。高级对话框在 `src/ui/advanced_search_dialog.cpp:559` 创建时即重新编译；筛选菜单则在只改一个字段后重新编译整个 spec。
- **影响**：用户只编辑不相关的条件也会扩大搜索范围，自定义条件从最终 query 中消失。这里存在真实的解析—修改—编译产品路径，并非单纯要求两个辅助函数在所有语法下互为逆函数。
- **建议**：在模型中保留可编辑的自定义条件或原始查询片段；无法无损表示的表达式，编辑其他字段时应保留其约束。不要只修对话框显示，因为筛选菜单和地址搜索也复用同一模型。
- **验证状态**：静态确定，未运行 UI。建议针对上述查询核对 parse/compile 后的约束，并分别验证高级搜索、类型筛选和继续输入搜索词。

## PC03 [P2] 批量重命名将只改变大小写的目标判为“未更改”

- **位置**：`src/model/batch_rename.cpp:205-209`；UI 消费点 `src/ui/batch_rename_dialog.cpp:303-308,319-324`。
- **触发**：例如 `report.txt` 通过替换规则得到 `REPORT.txt`，或者扩展名仅做大小写规范化。
- **证据**：模型使用 `_wcsicmp(new_name, original_name) == 0` 认定 Unchanged。UI 只将 `Ok` 计入待执行数，并以 `rename_count_ > 0` 启用应用，因此一批全为大小写变化时根本不能提交。
- **影响**：合法且有实际意义的文件名大小写调整被静默跳过，预览显示目标名变化却标记“未更改”。这是展示名称是否改变与目录中目标身份是否冲突两个不同判断被合并的结果。
- **建议**：Unchanged 使用大小写敏感的精确名称比较；目标冲突继续按文件系统规则判断，并让操作层显式支持同一文件的大小写重命名。大小写敏感目录还需要身份核对，不能只用不区分大小写的路径比较代替。
- **验证状态**：静态确定，未进行磁盘重命名。定向验证需包含仅大小写变化、确实完全相同、真实同名冲突，且保留原文件。

## PC04 [P2] 每次设备操作执行后都泄漏完整 Request 对象

- **位置**：`src/shell_host/main.cpp:416-420`；所有权转移在 `src/shell_host/main.cpp:729-743`；窗口分发在 `src/shell_host/main.cpp:1523-1526`。
- **触发**：任一次有效 `REQ_DEVICE_OPERATION`，包括设备条目的 copy、cut、paste、open、properties、newfolder；失败结果也走同一分支。
- **证据**：读取线程用 `make_unique<Request>()` 分配请求，成功 PostMessage 后 `request.release()`。窗口过程将裸指针交给 `ExecuteRequest`。该函数的设备操作分支调用并发送结果后直接 return，没有其他分支均有的 `delete req`，调用者也不删除。
- **影响**：长寿命 Shell broker 每执行一次设备操作，保留 Request、verb/name 以及最多 4096 个设备 ID 字符串。反复浏览或操作 MTP 设备使内存持续增长；批量长 PIDL ID 会放大单次泄漏。具体增长量未实测。
- **建议**：在 `ExecuteRequest` 入口统一取得 `unique_ptr<Request>` 所有权，并清理各分支的手工 delete，或对该分支补齐明确释放。统一所有权有助于防止以后早退路径再次遗漏，但不必改动其他模块。
- **验证状态**：静态确定，未运行设备操作或堆检测。建议用隔离的设备操作 fixture 覆盖成功和失败早退后的析构，并补一次真实设备往返验证。

## PC05 [P2] 完整 JPEG 预览没有读取 EXIF 方向，和缩略图路径的方向处理分叉

- **位置**：`src/preview_host/main.cpp:697-715`、`src/preview_host/image_decoder.h:8-11`、`src/preview_host/image_decoder.cpp:29-54,86-88`。
- **触发**：像素阵列按横向存储、依靠 EXIF Orientation=6 或 8 表示竖拍的 JPEG，分别用于缩略图和完整预览。
- **证据**：JPEG 嵌入缩略图路径将 `embedded.orientation` 显式传入 `DecodeImage` 并调整源尺寸。完整预览/普通解码调用省略参数，固定使用默认 `orientation=1`。完整解码器从 frame 获取尺寸和像素，从未读取方向元数据；只有 orientation 不等于 1 时才创建旋转器。
- **影响**：对使用 EXIF 方向的常规 WIC JPEG 帧，完整预览会按原始像素方向显示，可能和正确旋转的嵌入缩略图不一致；源宽高也按未旋转方向报告。**未用相机图片在此环境实测**，最终可见方向还需确认 UI 是否另有变换；本分区响应本身没有传递 EXIF 方向供 UI 补做。
- **建议**：让完整解码和嵌入缩略图共用方向语义，完整 frame 解码时读取适用的 Orientation 元数据，并统一尺寸、缩放、旋转顺序。不要只将缩略图的修正复制到另一个调用点。
- **验证状态**：代码路径分叉确定，可见结果待运行验证。建议使用像素角标明确的 EXIF 1/3/6/8 fixture，比较完整预览、无缓存缩略图和有缓存缩略图。

## PC06 [P3] 旧的 IFileOperation 删除实现已不可达，但仍保留整套结果和身份逻辑

- **位置**：`src/shell_host/main.cpp:478-602`，以及只服务该分支的 `ProgressSink`（`src/shell_host/main.cpp:178-339`）。
- **证据**：Reader 只将 device、legacy delete、authorized delete/recycle、new folder/file 交给 `ExecuteRequest`（`724-795`）。这些种类全部在 `416-475` 返回。后面的 IFileOperation 分支唯一能排入操作的 switch case 是 `REQ_AUTHORIZED_DELETE`（`525-529`），但该种类已在 `440-451` 返回。即使传入其他未知种类，switch 也只有 E_INVALIDARG。
- **影响**：运行时已使用 `DeletePermanentAuthorized` 和 `RecycleAuthorized`，同时仍维护约 280 行旧 sink、删除结果、身份核对、诊断与 IFileOperation 管线，容易让后续修复落到不会执行的实现，也会误导源代码审计和测试入口选择。这是可证明的失效实现，不把必要的 Shell/preview 进程隔离封装算作重复。
- **建议**：确认没有直接测试包含或特殊构建入口依赖后，移除不可达分支及只服务它的内容；保留实际仍被调用的创建、设备、上下文菜单路径。本文未自行删除。
- **验证状态**：静态调用分发确定。未构建；删除前仍需定向检查特殊宏构建和相关测试的源码依赖，不能机械移除整个 `deletion_identity` 模块，因为回收路径仍调用 `IsSameDeletionItem`。

## PC07 [P3] 文本层保留了另一套云占位判定，和公共文件读取契约分歧

- **位置**：`src/common/text_decode.cpp:9-21`，对照 `src/fs/cloud_state.h:22-29`；实际消费者包括 `src/index/content_search.cpp:208-212,230-234`、`src/index/content_index.cpp:722-733`。
- **证据**：文本层仅检查 RECALL_ON_OPEN / RECALL_ON_DATA_ACCESS，并以 PINNED 排除；现有 fs 定义包含 `FILE_ATTRIBUTE_OFFLINE`，且不因 PINNED 忽略仍未驻留的状态。属性 `FILE_ATTRIBUTE_OFFLINE` 单独存在，或 `RECALL_ON_DATA_ACCESS | PINNED`，两个函数返回相反。已有内容测试 `src/bench/content_search_test.cpp:331-332` 还将后一种旧语义固化为预期（此处仅检索命中，未将该测试文件算作完整阅读覆盖）。
- **影响**：不同读取入口对同一个属性集合采用不同准入规则；需要回源的数据有机会被文本索引/搜索视为本地。部分调用者还排除 reparse point，因此**不能据此断言所有云占位文件都会被下载**；真实风险需针对提供器的属性组合和调用路径验证。
- **建议**：确定并复用一个“读取是否可能回源”的属性契约，区分“用户要求固定在设备上”和“内容已经在设备上”。统一时同步修正旧断言，避免只增加第三个辅助函数。
- **验证状态**：契约分歧确定，云提供器行为未实测。建议用隔离属性模型覆盖 OFFLINE、RECALL、PINNED 组合，再用内容确未驻留的受控文件确认没有被动回源。

## 其他核对和未升级结论

- `common/crash_reporter.cpp:25-31,261-269,341-349` 的环形 breadcrumb 只有序号原子，payload 非原子，环绕复用与读取重叠时不存在安全快照。当前产品 `AddBreadcrumb` 的可见调用仅在 UI 致命异常入口；缺乏实际并发写入/环绕调用证据，本次不将通用 API 的潜在调度风险升为独立 P2，也不声称已复现崩溃。
- `preview_host/shell_thumbnail_bitmap.h:32-37` 在正常 provider 分支分配像素前未设最大尺寸，cache-only 分支反而在 `main.cpp:599-605` 有请求尺寸检查。异常提供器可能放大隔离宿主内存；未证明系统提供器会违反尺寸契约，作为边界记录，未报确定产品缺陷。
- Shell namespace 的新 `::pidl:` ID 在图标、设备操作和菜单路径使用统一 `shell_id`，而 metadata/launch 部分仍直接使用解析名称。未确认有实际调用方把这种 ID 送入不支持的接口，不能泛化成所有 MTP 功能失败。
- Context/launch/query helpers 的有限进程池、作业对象、超时、取消、断线与退休机制承担第三方 COM/驱动隔离；它们的局部 Handle/STA 包装不能仅因外形相似就认定为可删除重复。没有运行第三方扩展 query/invoke/close，也未把释放调用存在解释为资源已在真实扩展中释放成功。
- `integration_host/main.cpp` 为已有提权批处理适配器的 9 行入口，未发现可独立确认的问题。

## 覆盖与验证边界

- 完整阅读：104/104；`common` 32、`ipc` 14、`model` 14、`preview_host` 13、`shell_host` 30、`integration_host` 1。
- 本次验证：当前源文件完整阅读、当前 diff 审阅、针对上述问题的发送/解析/执行/显示消费链追踪、覆盖 JSON 的语法及目录计数检查。
- 没有执行：编译、单元/自测、安装、真实 MTP/SMB/云盘、相机 JPEG 可视预览、第三方 Shell 扩展、堆或并发检测。上述缺口均保留，未把静态推理写成运行通过。
