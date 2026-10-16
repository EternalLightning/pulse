# Pulse 未尽审查事项收口（2026-10-16）

来源：[原始未尽清单](code-audit-outstanding-2026-10-16.md)。本轮已完成 U01–U12、R01–R08 的局部修复或契约核实及定向验证，补齐原 App 28 项、UI 18 项全文阅读缺口，并建立 Ops/FS/Shell 66 个生产文件的逐文件台账。真实环境和视觉验收仍有下列边界；本报告不是全项目验收或发布批准。

## 逐项结果

| ID | 处理结果 | 实际验证与依据 |
| --- | --- | --- |
| U01 | 持久化全局 Add ID、跨卷按身份重放；旧/坏日志保留并要求重建 | Delta 25/0；含再次追加/启动、FRN/checkpoint、伪造范围拒绝、canonical/letter V1、隔离重建发布与重启；原实现有效失败对照 10/3 |
| U02 | mmap 及内存重建验证父链、卷布局；离线保留父先子后 | mapped 24/0、parent-cycle 14/0，structure 覆盖移动后离线 FRN 保留 |
| U03 | 父节点变化后禁用旧 DFS scope 快捷路径 | structure 71/0 的移动/改名/新增后代与父链 oracle |
| U04 | posting 与 patch 候选去重 | 同一 structure selector 的稀疏/密集、拼音、total/分页 |
| U05 | 保留完整深路径，拒绝未解析/循环 FRN | 同一 structure selector 的 48/64/256/300 层、父先/子先及排除 |
| U06 | 排除快照在同一 mutex 下发布 | exclusions-concurrency 5/0，10 次实际隔离重建及并发读者 |
| U07 | 否定整个 file/folder 名称及类型谓词 | query-boundaries 53/0、folder-kind-negation 32/0、content filename 17/0；Engine 实际集合 20/0 |
| U08 | 统一规范化完整路径谓词及绝对路径组件边界 | 同组 query/content/Engine 检查，包含 skip/skip2、多绝对条件及相对路径 |
| U09 | 严格 JSON/Unicode，区分六类加载结果；错误后禁止覆盖原文件 | network-config 60/0 |
| U10 | 部分枚举失败保留命中并报告不完整 | transient-errors 9/0、task-enumeration 11/0 |
| U11 | 16 查询连接加控制实例，饱和拒绝而保留已有查询 | agent-capacity 11/0；取消后恢复接入、其它订阅存活 |
| U12 | 重建命令传播 SCM 失败及非零退出码 | host service 5/0；上层 bool/失败反馈静态核对，未控制真实服务 |
| R01 | roots 非空时覆盖兼容 root | 生产 wire、缓存、task 双根/单根 content-contract 6/0 |
| R02 | 暂时属性失败阻止错误 prune | shared-retention 12/0；恢复更新和真实删除对照 |
| R03 | 原 UI 已消费错误；统一不完整根状态 | root-errors 6/0；不将原行为误写成“UI 隐藏错误” |
| R04 | 迁移全程持目录/文件租约，按拥有句柄回滚；不支持元数据拒绝迁移 | identity 6/0，metadata 17/0，默认专用迁移 Medium 副本 30/0；原错误回滚对照 4/2 |
| R05 | 区分正常 EOF 与枚举/属性错误，失败不写成功 manifest | enumeration-errors 10/0；原实现 3/6，含部分副本已生成后失败 |
| R06 | 批量重命名预览单 worker、最新代际、取消及关闭不阻塞 | batch_rename_preview 9/0 |
| R07 | watcher 会话邮箱和 stop HANDLE 真实所有权 | shell_registry_watch 8/0；超时后 worker 完成与会话更替 |
| R08 | 数值溢出/非法日期拒绝；机器 JSON 与 SQLite/sidecar 失败不退回默认扫描 | query 53/0；host machine 25/0、content 24/0；content-config-errors 6/0；Engine 调用者严格失败处理 |

各表项有共享 selector，不能逐行相加。并非所有项目都撤回生产修复执行了红对照；缺少红对照时，依据为原代码路径和生产接口上的故障/边界断言，详见各分报告。

## 补读发现及设计检查

- App：原 28 个文件全文阅读已补齐；另检查新增 helper。修复打开所在位置选择意图被导航清空、分屏输入/绘制间距偏差、色块动作 ID 冲突、菜单跨模态向量元素指针悬空，以及空 tags 复活/损坏配置覆盖。App 独立检查 9+8+27 个断言；真实菜单/拾色器及注入列表交互 fixture 39/0。
- UI：原 18 个文件全文阅读已补齐。修复 popup 工作区拟合/子菜单锚点、窄详情面板、preview 关闭后滚动高度、DPI 字体、超过 96 个文本命中范围、冷关联 COM 探测移至已有 worker，以及明确的高对比颜色偏差。五个定向 selector 合计 72/0；没有进行截图视觉批准。
- Ops/FS/Shell：66 个生产文件逐文件台账，包含原 Detailed 16 个 cpp 补读。修复目录 Skip 元数据收尾、日志 snprintf 越界读取和回收站 v1 路径缺 NUL。定向 17+3+6 个断言通过。
- 原生 EDIT/IME、系统选择器/Shell 对话框、辅助功能 tooltip、第三方预览窗口和图形失败降级保留有明确平台职责的例外；复用既有 hosted edit、Painter、布局和生命周期模型。没有为表面统一新增控件体系或合并无依据的抽象。

详细台账：[Engine](code-audit-engine-closure-2026-10-16.md)、[查询](code-audit-query-closure-2026-10-16.md)、[网络](code-audit-network-closure-2026-10-16.md)、[内容](code-audit-content-closure-2026-10-16.md)、[Host/配置](code-audit-host-config-closure-2026-10-16.md)、[存储/诊断](code-audit-storage-closure-2026-10-16.md)、[App](code-audit-app-closure-2026-10-16.md)、[App 大文件](code-audit-app-large-closure-2026-10-16.md)、[UI](code-audit-ui-closure-2026-10-16.md)、[Ops](code-audit-ops-closure-2026-10-16.md)。阅读计数针对原清单，不推断当前全仓全部文件覆盖率。

## 构建与证据

受影响生产目标 `pulse`、`pulse_index`、`pulse_shell` 及相关独立测试目标构建成功；最终构建无新增编译警告。`git diff --check` 通过，仅有现有 LF/CRLF 转换提示。没有执行全量 Release、完整 selftest 或所有历史测试。

最终分组通过断言：Engine 159、独立 query 85、network 60、content 88、host/config 54、storage/diagnostics 63、App 44+39、UI 72、Ops 26，合计 **690 PASS / 0 FAIL**。包含前置/清理断言，同一 selector 重跑只计最终一次，content filename 17 已包含在 content 88 中，不重复计入 query。默认迁移结果绑定报告所列确切哈希；其后增加 wallpaper 测试的 selector 二进制单列记录，不混称同一个可执行文件。

本轮原始日志在 [audit-closure-2026-10-16](audit-closure-2026-10-16/)；内容组在 `bench_data/content-audit-closure-2026-10-16/green-*.log`。旧失败日志保留。首次 UI 编译 small 宏冲突已修正；查询输出的宽流 locale 中断已改 UTF-8 即时输出。Engine 查询初次三条失败是漏算 scope 自身，补精确诊断及空查询全集断言后通过；App fixture 未启动 worker 导致 generation=0 的注入错误也先修前置条件，没有弱化行为断言。

默认迁移测试的仓库 Low 完整性副本拒绝 HKCU locator 保存；哈希一致的新建副本实际 child token RID 8192（Medium）运行 30/0，支持权限环境解释。未更改产品 UAC 逻辑、原 exe 或仓库 ACL；早先无输出异常的具体抛出点没有反推为已查明。

内容容量初版夹具仅改 LOCALAPPDATA，未隔离 KnownFolder/HKCU locator，初始化超时前可能读取真实 sidecar。最终已改显式隔离 DB/agent child。没有运行前 hash；现有 sidecar 时间早于本次、事后 hash 支持未重写，但不能证明绝对未写。完整事实与路径在内容报告，不声称全程从未读取用户配置。

## 尚需实际环境验收

本轮代码清单与隔离验证已收口，以下仍未执行，不能计为通过：真实多卷 USN/卸载、断电/磁盘损坏、SMB 断连和 RaiDrive/云盘阻塞、真实 SCM/UAC/服务重建、Office/PDF/外部 Shell provider，以及暗亮/高对比、跨屏 DPI、辅助功能和窄布局截图。几何、DirectWrite 或屏幕外窗口夹具不等于完整视觉及端到端验收；并发压力不代替竞争检测证明。

没有提交、推送、安装、发布或部署；既有 dirty 修改保留。本轮未主动改变真实用户偏好或服务。只清理已核对归属的两个旧 Delta 失败夹具及其空临时目录，未广泛清理 bench_data。
