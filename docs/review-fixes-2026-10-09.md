# Shell、后台 CPU 与关联代码风险修复

基线：`cac5f32`。本轮先复核调用链，再用定向 fixture 验证触发条件；不以编译成功代替功能验收。

## 已确认的问题与修复

| 项目 | 确认依据 | 修复边界 |
|---|---|---|
| 索引 Writer 空转 | 隐藏隔离实例在 16 逻辑处理器上占用 6.11%；生产 Writer 定向回归中，断线卷请求、搜索、取消及失败写入占满约 94–100% 单核 | 等待同时要求连接与待处理请求；连接/停止状态和条件变量同步；失败写入保留请求并退避；重连重新获取卷范围 |
| 部分 Move Undo 清理删除替换目录 | 真实 Move/Undo、逐项移动与清理前替换目录，旧实现删除空替换目录并消费未解决历史 | 用枚举时的原对象身份清理；身份变化保留历史但禁用自动重试，不补采替换对象身份授权 |
| 列式共享请求被导航取消 | 阻塞实际 worker，让另一 pane 的 ancestor 列借用主列表请求；旧实现在导航及同路径重载后均丢失原代际结果 | 取消检查计入列式消费者；同路径重载复用仍被借用的请求 |
| 缓存右键行提前点击无效 | 缓存行不具备当前会话执行身份，原实现却显示为启用 | 缓存行禁用；按当前会话的确切身份映射已就绪行；维持冻结布局与严格执行校验 |
| 挂死右键扩展阻塞其他项及会话回收 | 原会话等待所有 handler 完成，关闭后无限 join；测试 fixture 注入不返回的 handler | 每会话独立可终止进程；已完成 partial 项可执行；上限按实际存活进程计算；文件操作宿主保留；正常可见窗口保活 |
| 拖出文件在 OLE 循环前同步调用 Shell provider | 实际 UI 调用链与阻塞准备 fixture | 自有 IDataObject 即时提供路径及 effect；可选 CIDA 在单个有界后台 STA 准备；未就绪不宣称支持，不阻塞拖放或关闭 |

## 实际验证记录

- CPU 定向入口 `pulse_search_client_routing_test --writer-idle-only`：19 项通过；修复后各观察窗口内 Writer CPU 为 0.000% 单核，保留请求、停止唤醒和隔离管道重连交付均通过。该数值受 Windows CPU 计时粒度限制。
- 已构建主程序同条件复测：隐藏隔离实例 10.013 秒消耗 0.09375 秒 CPU，在 16 逻辑处理器上约 0.0585%，基线约 6.11%。测试进程及其确切工作区 helper 子进程已清理。
- 撤销入口 `pulse_ops_test --filter move-undo-source-identity`：10 项通过，覆盖替换对象保留、禁用错误重试及正常合并 Undo 对照；隔离 fixture 已清理。
- 共享请求入口 `pulse_directory_contract_application_test --shared-requests-only`：10 项通过，覆盖两主列表及列式消费者的导航/重载与原 generation 交付；未知参数退出码确认为 2。
- 拖出入口 `pulse_drag_drop_test --drag-out`：12 项通过，覆盖阻塞准备、立即进入拖放循环、线程数量有界、过期结果隔离及 effect/取消语义。
- 真实 Shell 入口 `pulse_drag_drop_test --shell-compat`：10 项通过，实际复制和移动成功，完成的 CIDA 可解析，回收站悬停接受 MOVE。回收站未实际执行删除。
- 菜单 controller 入口 `pulse_app_controllers_test --context-menu-safety`：31 项通过；菜单 model 入口 `pulse_menu_text_test --command-state`：3 项通过，覆盖缓存禁用、partial 身份映射、旧数字 ID 复用与原位启用。
- 真实进程入口 `pulse_context_session_test <pulse_context_worker_fixture.exe>`：28 项通过，覆盖慢/挂死 handler、已完成项执行、关闭后物理回收、4 个存活进程上限、父进程退出清理、非模态窗口保活、启动的外部程序保活，以及原文件操作宿主仍可 PING 和真实创建隔离目录。
- 受影响目标与主程序最终增量构建成功，无新增警告；`git diff --check` 通过。

首次共享请求基线使用的旧 fixture 二进制尚未包含 selector，实际执行了打包入口并复现两项失败。后续加入未知参数拒绝，并只用独立入口完成相关验收。

拖放真实 Shell 验收中，工作区 EXE 的实际进程 token 为 Low（RID4096）；最终使用相同字节的独立 TEMP 副本，进程自身确认 Medium（RID8192）。初次复制/移动失败在旧系统 IDataObject 对照中也复现，确认为测试模拟漏传 DragEnter 的 `MK_LBUTTON`；修正测试协议后两项通过，未据此更改生产行为。临时副本和隔离文件均已清理。

菜单进程验收的工作区 runner 与实际文件操作 fixture 同样为 Low（RID4096），28 项中只有创建隔离目录失败，HRESULT 为 `0x80070005`。将相同字节写入独立 TEMP 目录、确认 runner/fixture 的 SHA256 均一致且实际 token 均为 Medium（RID8192）后，同一入口 28 项全部通过；未删除断言或修改原文件 ACL。受限工具宿主曾出现三项测试退出 1 且零输出，后续使用正常宿主运行并记录原生退出码。

用户随后报告工作区程序无法打开图片且关闭按钮无效，并亲自确认移到其他目录后两项恢复。工作区 `build` 与 `Pulse.exe` 的 ACL 均继承 Low mandatory label；没有因此修改图片激活或关闭逻辑，临时图片诊断入口已撤销。

## 便携包验收

- 仅构建五个 Release 生产目标，保留 `PULSE_WITH_SELFTEST=OFF`；版本检查与静态 MSVC 运行库依赖检查通过。
- 产物：`dist/Pulse-1.0.6.0-portable-win-x64.zip`，11,498,004 字节，SHA256：`b5d0a4ccad4eb1f255b7e90ca48a52e09f160670315bccf52f649be26acbfe7b`。
- 在普通宿主的独立 TEMP 目录解压，24 项验收通过：九个运行时文件与当前构建哈希一致、说明和六组许可证齐全、两个独立配置实例均确认实际 Medium（RID8192），关闭到托盘及恢复正常，禁用托盘后关闭按钮可正常退出且退出码为 0。
- 验收只使用隔离配置、测试进程和临时目录，均已清理。ZIP 不携带 NTFS ACL；解压到 Low 标签目录仍会继承该目录权限，应解压到普通用户目录运行。

## 验证限制

- 不运行完整 release 构建、全量 selftest 或所有历史测试。
- 不安装服务、不改变用户偏好、不操作用户文件或清空回收站。
- 回收 provider 的既有身份校验与内部按路径操作之间的竞争窗口不在本轮修复范围。
- 单个可选 CIDA provider 若永久阻塞，会占据唯一准备槽；后续拖出仍使用即时路径格式，不继续累积线程。
- 慢拖放 provider 使用可控阻塞 renderer 模拟；未覆盖所有第三方 Shell 扩展。
- 菜单状态验证覆盖 controller/model 与真实 helper 进程；未完成交互式外观验收。冻结布局下新增行仍待下次打开菜单呈现，已存在且身份匹配的行可原位启用。
