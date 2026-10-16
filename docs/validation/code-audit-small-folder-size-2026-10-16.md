# 目录大小小批次审查（2026-10-16）

子代理调用因 502 失败，重试中断后由主代理接管。实际全文阅读 folder_size_index.h（81 行）、folder_size_protocol.h（26 行）、index_folder_sizes.cpp（51 行），均位于 src/index，当前 diff 为空。未确认新缺陷，未改生产代码、未构建或执行测试。

- Build 使用自叶至根队列聚合，检测自父节点、无效父节点、父节点非目录、环路和 uint64 溢出，失败不发布有效 totals。生产读取回调 FolderSizeItem 对非法 id 返回空 Item；不将模板在任意恶意回调下的行为视为当前可达问题。
- Replace 按旧、新祖先链分别减/加，检查下溢、上溢和循环深度；无法维护时 Reset 使后续重新构建。原有 fixture 已包含文件增长/缩短、创建、移动、子树移动及重复删除，本轮只阅读测试，不记作重新通过。
- 查询在索引未就绪、构建中、日志 gap、离线卷、未覆盖/排除路径时返回 unavailable，区分已知零值。授权分支重新搜索可见子项，超过 4096 条时不宣称精确总量。
- 协议读取校验版本、期望数量、128 上限、布尔范围和尾部字节，成功后才替换输出。host 请求检查数量和路径长度，服务返回前校验 CallerVisibility 的目录状态/Incomplete；不能只看授权回调返回 false 就认定返回了错误零值。

局部依赖：index_engine_test.cpp 前 145 行的既有 folder-size fixture；index_host.cpp 的 kFolderSizeRequest 处理。未全文审查 Engine/Search/CallerVisibility。本轮没有动态测试并发索引状态变化、真实权限及大目录分页；覆盖不等于端到端验收。
