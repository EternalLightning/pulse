# 索引 host、client 与 MFT 全文审查（2026-10-16）

开始时阅读 AGENTS.md、工作区状态和本组 diff。保留全部已有 dirty 修改，包括 index_mft.cpp 的 USA 验证改动。全文审查后，主代理授权本分工只修复 HOST-2；HOST-1 保留为待修复发现。没有启动服务或修改用户配置。

## 全文覆盖

| 文件 | 本轮完整阅读范围 | 检查重点 |
| --- | --- | --- |
| src/index/index_host.cpp | 1–1228 | helper/service 启动、SCM 命令、pipe 安全与 accept、读写队列、搜索订阅/取消、change/feed/folder-size 请求、token 生命周期、停止与 join |
| src/index/index_client.cpp | 1–588 | 连接验证/重连、共享连接所有权、读写取消、pending/subscription 路由、响应解码、提升命令及返回值 |
| src/index/index_client.h | 1–126 | 公共生命周期契约、线程/锁/句柄及状态成员 |
| src/index/index_mft.cpp | 初始 1–356 全文；最终 1–365，修复 diff 再审 | NTFS 记录/属性/USA/runlist 边界、物理读偏移、批量读取、错误/取消/截断状态 |
| src/index/index_mft.h | 1–41 | 回调、I/O 注入与完成状态契约 |

## 确认缺陷

### HOST-1 [P2] 重建命令忽略服务控制失败并返回成功

- 位置：index_host.cpp:1119–1124；ReloadService 的失败分支在 1075–1087。
- 普通可达触发：服务未安装或已停止时执行设置中的“重建索引”；运行中的服务在命令到达前停止，也可触发。settings_controller.cpp:557–579 没有用 service state 拒绝 rebuild，174–175 将其交给 RebuildElevated。
- 路径：`--rebuild-index` 只置 `ok=true`，随后调用 ReloadService，但无条件退出 0。OpenService 或 ControlService 失败不会传回。IndexClient::RebuildElevated 经 RunElevatedIndexCommand 将退出 0 解释为成功。
- 影响：用户收到成功结果，实际没有投递到 Engine::RequestRebuild；这个分支也未保存一个供下次启动执行的重建请求。卷/排除配置虽会持久化，但本条结论限于完全没有持久化动作的 rebuild。
- 修复方向：保留具体 SCM 错误并返回；若产品要在停止服务时支持重建，先显式恢复服务并验证控制请求成功。仅成功投递请求仍不等于重建工作已经完成。
- 证据为静态调用链；没有操作真实 SCM 或弹出 UAC。

### HOST-2 [P2] MFT runlist 无法解析后仍可把猜测的连续读报告为 Complete

- 位置：index_mft.cpp:295–302、353；DecodeRuns 在 91–120 逐项向输出追加。
- 普通错误触发：record 0 的 USA 不匹配、属性边界损坏或 runlist 后部损坏，同时其他 FILE 记录仍可读取。新增 USA 检查会正确拒绝该 record 0，但调用者随即进入连续物理布局猜测。
- 路径：ParseMftRecord0Runs 失败时，代码从 MftStartLcn 构造覆盖整个 MftValidDataLength 的连续 run；NTFS 元数据并未证明该范围连续。只要猜测范围中有至少一条可解析的 FILE 记录，最终 `count>0` 就返回 Complete。
- 更直接的身份风险：DecodeRuns 可能先追加若干有效 run，再因后部无效返回 false；调用处没有清空 `runs`，而是在已有前缀后再追加连续 fallback。相同物理记录可再次按已递增的 file_off 生成另一 record index / FRN。即使先清空前缀，也不能证明连续布局正确。
- 上层影响：index_engine.cpp:2947–2966 仅在 Failed 时丢弃本次集合并调用 FSCTL_ENUM_USN_DATA；错误 Complete 会跳过该回退，之后将非空集合交给 BuildMftTree。故这是能污染索引完整性和对象身份的错误接受路径，不只是单条损坏记录被跳过。
- 修复方向：无法验证 record 0/runlist 时返回 Failed，让既有系统枚举回退接管；发布 Complete 前还应核对读取映射覆盖所需逻辑范围。未支持的扩展映射不能默认为完整。
- 修改前 MftReadFixture 本身用不含 DATA runlist 的 record 0 作为成功基线，不能证明上述映射失败处理正确。已将该函数改为可解析的非连续 DATA runlist，并保留原来的读错误、取消及 cap 断言。

## HOST-2 最小修复与定向验证

- record 0/runlist 无效或为空时立即返回 Failed；不再构造连续 fallback，也不会复用解析失败留下的 run 前缀。
- 要求首个 DATA extent 从 VCN 0 开始；对所有解码 run 的字节覆盖量作溢出检查，覆盖不足、空或非 record 对齐的 MftValidDataLength 均在 emit 前失败。读取范围以有效逻辑数据长度为界，忽略映射末尾的已分配 slack。
- MftReadFixture 使用两个物理不连续区间，检查两个有效文件记录的逻辑 FRN；新增 record 0 USA 损坏、有效 run 前缀后损坏、空映射、范围不足、非零起始 VCN、非记录对齐长度、有效长度之后已分配尾部的用例。读错误、consumer cap、取消与夹具清理检查保留。
- Red：保留原有 USA 修复，仅撤回本轮 fail-closed hunk，构建 pulse_index_engine_test 后运行 `--mft-read-only`：10 PASS / 7 FAIL，退出 1。失败恰为上述七类错误接受；正常非连续布局及原有读失败/取消/cap 检查通过。
- Green：恢复本轮 hunk、重新构建同一目标，明确重新编译 index_mft.cpp 并链接，退出 0，输出无编译警告；同一 selector 最终 17 PASS / 0 FAIL，退出 0。夹具清理通过，相关 `git diff --check` 退出 0。
- 环境：初次未授予构建槽的尝试在编译输出前取消；正式排队后的沙箱内构建在 VerifyGlobs 阶段无后续输出，亦取消。内容相同的定向构建经批准在沙箱外成功；沙箱内 selector 退出 1 无输出，不能作功能失败解释，沙箱外 red/green 得到上述完整输出。没有运行其他 selector 或完整测试程序。

## 其余核对与边界

- Client 的 overlapped I/O 通过 shared_ptr 保持 Connection 存活到完成，Disconnect 先取消再等待 writer；未将连接 reset 误报为立即关闭仍在读的 HANDLE。
- Host drop 先取消 I/O，再取得 write_mu 关闭 pipe；主退出先停止接受/客户线程与搜索线程，再停止 Engine，最后释放 helper singleton。没有重复既有报告已核对的 checkpoint 顺序。
- 搜索 session 上限、队列字节/条目、replace 与取消标记、响应上限均已检查。service 搜索从捕获的 caller token 授权并在入队前复查结果；不将 SYSTEM 索引拥有全部元数据直接等同于返回全部元数据。
- Host accept 在非首实例 CreateNamedPipe 失败时重试，未套用 content host 的“第 16 个连接导致退出”既有缺陷。首次实例失败的 helper/service 退出/重试策略已阅读。
- MFT 记录长度、属性 resident 边界、USA 尾词比对、算术溢出、读失败/取消/consumer cap 区分均阅读；未改动既有修复。

## 局部依赖及验证限制

为确认上述路径，局部阅读 index_engine.cpp:2925–3035（IndexVolumeMft 和 BuildMftTree 入口）、settings_controller.cpp:150–180、557–579、index_engine_test.cpp:786–856；读取 index_server_auth.h、index_request_limits.h、index_service_start.h 作为直接接口背景，但不计入本轮五文件全文覆盖清单。index_transport_security.h 仅定位 CaptureCaller/RevertOrFailFast/VerifyServer 符号，未全文复审。参考本日已有 shard/cache、change tracking 和小型 IPC/边界报告以避免重复登记。

HOST-1 仅静态代码证据。HOST-2 的失败与修复后结果来自隔离文件和 MftReadIo，不是完整服务或真实卷故障注入。没有验证真实 NTFS 卷、ATTRIBUTE_LIST 扩展映射、SCM/UAC、权限变化竞争、服务重载或 UI 反馈，没有做性能测量或资源耗尽注入。没有对全文未覆盖的 Engine、CallerVisibility、公共 pipe I/O、Shell interaction、网络/content agent 实现作完整性结论。
