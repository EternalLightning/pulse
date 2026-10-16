# 网络索引完整代码审查（2026-10-16）

范围：仅本次分配的 8 个文件；本组 `git diff --stat` 与 `git status --short` 均无修改。已读取根 AGENTS.md 及已有 coverage 记录；不修改产品代码、不构建、不连接真实网络、不写用户配置。以下为静态确认，复现步骤是建议验证方案，未冒充已执行测试。

## 全文覆盖

- src/index/network_agent_client.cpp（420 行）：代理启动/身份校验、串行 RPC、超时取消、搜索会话路由、实时枚举线程、状态/根管理。
- src/index/network_agent_client.h：公开接口、锁域、工作线程与生命周期。
- src/index/network_agent_host.h：生产/fixture 入口。
- src/index/network_agent_main.cpp（350 行）：协议解码、搜索序列化、变更租约、连接接纳、singleton/空闲退出。
- src/index/network_agent_protocol.h：端点身份与帧类型/上限。
- src/index/network_index.cpp（1198 行）：配置、分片写入/映射、规范化、抓取、监听、变更记录、检索。
- src/index/network_index.h：状态/线程及公开接口。
- src/index/live_filename_search.h：UNC 判别、查询匹配、递归枚举、增量全局排序分页。

## 发现

### NET-1 [P1] 枚举中途失败仍将残缺分片发布为已同步并删除旧分片

位置：`NetworkIndex::BuildRoot`，network_index.cpp:815-821、844-845、868-895。

触发：已有完整索引后，某子目录不可访问（FindFirst 返回 ACCESS_DENIED/网络错误），或任意目录 FindNext 中途返回网络错误。当前只在根目录 FindFirst 失败时设置 failure；子目录失败直接 continue，FindNext 退出完全不检查 ERROR_NO_MORE_FILES。故 failure 仍为 SUCCESS，保存/替换新分片、状态显示“已同步”，再删除旧分片，之前可搜索的条目静默丢失。现有 storage_failure/双槽保护只覆盖存储写入失败，不能保护扫描不完整。

最小验证：注入枚举接口，先给出含两个子树的完整扫描，再让第二子树 FindFirst 返回 ACCESS_DENIED；另例在一个文件后令 FindNext 返回 ERROR_BAD_NETPATH。断言旧分片不替换、不删除且显示同步失败。全部可在隔离目录/假枚举器下完成，不需要真实 SMB。

最小修复：记录全部非正常枚举终止的错误，FindClose 前捕获 FindNext 的错误；区分空目录 ERROR_FILE_NOT_FOUND/ERROR_NO_MORE_FILES 与真正失败，失败时保留旧分片。不能简单把空根视为离线。

### NET-2 [P2] 映射盘转换检查了错误的错误通道

位置：`NormalizeNetworkRoot`，network_index.cpp:465-466。

WNetGetUniversalNameW 直接返回状态码；代码丢弃返回值，改读 GetLastError。容量探测返回 ERROR_MORE_DATA 但线程 LastError 非此值时，不执行第二次调用，后续 UNC 检查将合法映射盘路径拒绝。设置入口最终调用 AddRoot，因此文件夹选择返回映射盘路径时可触发；并非只有内部调用。

最小验证：用 stub 让首调用返回 ERROR_MORE_DATA、设置 bytes，但保留 LastError=ERROR_SUCCESS；第二调用提供 UNC。当前返回空串，修复后返回对应 UNC。现有测试只涵盖 UNC/普通本地盘，没有此例。

最小修复：保存首次 WNetGetUniversalNameW 返回值，并比较该值与 ERROR_MORE_DATA，保持第二次调用现有 NO_ERROR 判断。

### NET-3 [P2] 网络根校验接受扩展本地设备路径

位置：`NormalizeNetworkRoot`，network_index.cpp:461、475-483；`NetworkIndex::AddRoot` 使用其结果。

输入 `\\?\C:\fixture` 不属于 extended UNC，不会转写；又满足以两个反斜杠开头、非空 server/share 的宽松检查，原样接受。BuildRoot 的 LongPath 对该前缀原样保留，因此网络索引能够保存并递归扫描本地目录；`\\.\...` 也没有被明确拒绝。与同组 FilenameSearchRoot 对设备路径的显式拒绝不一致。

最小验证：仅调用 NormalizeNetworkRoot，断言 `\\?\C:\fixture` 与 `\\.\C:\fixture` 返回空，正常 `\\?\UNC\server\share` 仍转为普通 UNC；无需实际扫描。

最小修复：extended UNC 转换后，显式拒绝剩余 `\\?\`、`\\.\` 命名空间，再进行 server/share 判断。

### NET-4 [P2] 损坏配置被当作成功的空根列表，后续添加可覆盖原配置

位置：`ExtractStringArray`，network_index.cpp:197-234；`LoadNetworkRootsFile`，502-526；`Start`，640；`AddRoot`，706-710。

LoadNetworkRootsFile 仅要求文件出现左花括号；ExtractStringArray 用空 vector 同时表达合法空数组、缺键及解析失败，且不要求数组/对象闭合。例如 `{"roots":[123]}` 直接作为加载成功且空列表；Start 也忽略加载失败信息。用户随后添加一个根时，SaveNetworkRoots 从当前空 roots_ 生成新配置，覆盖原损坏配置而没有提示。合法 JSON 中的 `\uXXXX` 也不会按 Unicode 转义解码，会被转为字面 uXXXX 路径。

最小验证：隔离文件分别写入非字符串 roots 项、截断数组、合法 Unicode 转义 UNC，检查加载错误/正确解码；不要调用使用用户配置目录的集成入口。再以配置注入方式验证加载失败后不能无提示覆盖。

最小修复：解析函数区分失败与合法空列表，校验结构并正确解码转义；Start 保留并暴露配置加载失败状态，AddRoot/RemoveRoot 不应静默覆盖未成功读取的配置。此项涉及配置错误语义，建议单独修复而非混入枚举改动。

## 已检查的现有防护与验证限制

- 客户端实时搜索以 session/id 拒绝过期发布，Stop 设置取消事件并 join；RPC 有帧上限、deadline 和身份验证调用。
- Agent 搜索使用内部 id 和 timed_mutex，连接数有上限；不将这些静态观察表述为安全渗透通过。
- 分片 Open 检查文件大小、记录与池范围、每项 path/name 范围；BuildRoot 的存储失败分支保留旧 shard。
- live_filename_search 对异常枚举已有 incomplete/error 标记，未发现与 NET-1 同样的静默成功路径。
- 未执行任何测试或构建。读取的现有 index_test_main 网络集成入口会访问真实根并写用户网络配置，不适用于本次只读授权。未以现有测试源码的断言数量声称通过。

## 局部依赖（不算全文覆盖）

- src/bench/index_test_main.cpp：网络集成与规范化/UTF-8 roundtrip 区段。
- src/bench/search_client_routing_test.cpp：实时扫描与会话路由测试区段。
- src/app/settings_controller.cpp：AddNetworkPath/RemoveNetwork 异步任务入口区段。
- src/index/search_result_merge.h/.cpp：扫描结果归并语义（定向读取）；src/index/index_engine.h 的页大小常量。
- CMakeLists.txt：网络安全测试目标位置；src/index/change_tracking.cpp 仅符号定位，未进一步审查。
- docs/validation/code-audit-core-coverage-2026-10-15.md：确认上述 8 文件旧记录只有清单/模式搜索。

源码未改。以上待主代理确认后按授权处理；不自行扩大范围。
