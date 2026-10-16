# 文本任务读取小批次审查（2026-10-16）

子代理两次调用未提供可用结果后中断，由主代理接管；全文阅读 src/index/text_task_reader.cpp（298 行）和 text_task_reader.h（46 行），当前 diff 为空。未确认新缺陷；未修改代码、构建或执行测试。

- 每次读取重置结果和解码状态；取消释放可复用 scratch。空 needle/最大大小、unsafe 文件属性和短读有明确失败分支。preflight 使用实际读取句柄的 metadata，跳过不发布 version，callback 后再次检查取消。
- UTF-8/DBCS/GB18030 分块保留未完成字符；UTF-16 BOM 分支先拒绝奇数字节。搜索尾部保存 needle 长度加上下文，跨块匹配保留行号；上下文切边避免主动切断完整代理对。
- 强制 UTF-8/BOM 仍继续验证后缀；Auto 提前返回要求两个可能解码路径已有相同命中与行偏移，或主解码已失败且 fallback 命中。没有将有意提前命中优化误报为一定需要读完整文件。
- 读取结束校验句柄大小/mtime/ChangeTime，再重开路径比对卷与文件 ID/ChangeTime，变化返回 ERROR_RETRY；不把不同路径对象的正文当作稳定结果。
- case folding 的 locale 必须在单次查询中不变，接口已写明。字面读取仅用于正向单字面/短语候选，不替代复杂查询和完整索引提取。

局部核对 content_task_search.cpp 的 worker 初始化及调用，text_task_reader_test.cpp 中跨块 UTF-8/UTF-16、空文件、奇数字节、强制编码后缀和 Auto fallback 对照；这些仅是现有测试覆盖证据，本轮没有重跑。未动态验证真实文件替换、网络/云提供程序、取消延迟或任意系统代码页；未全文覆盖 content_task_search.cpp 与 text_decode.cpp。
