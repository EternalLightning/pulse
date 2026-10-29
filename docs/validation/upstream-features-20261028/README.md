# 上游功能适配与定向验证（2026-10-28）

上游基准为 [jimmgreen/pulse 的 be50f0f4](https://github.com/jimmgreen/pulse/commit/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26)，对应 1.0.58 源码。保留本地多窗口、操作中心、配置存储、文本渲染以及已有未提交修改；按模块适配，没有合并整条上游历史。

## 本次实现

| 请求 | 本地接入与适配 |
| --- | --- |
| 网络位置 | 后台 mDNS/WSD 发现，侧栏与网络位置页展示；连接服务器支持 UNC、SMB URL 和 IP，沿用现有 Windows 认证及代次校验；访问成功后由共享后台存储记录最近服务器/共享。系统 NetHood 快捷方式只读取本地元数据，不解析远端目标。 |
| 手机 | 独立 STA 设备源、插拔通知、锁定/断开/超时状态；此电脑与侧栏设备入口、设备目录浏览；复制、粘贴、新建文件夹与打开交给后台 Shell。系统菜单保留设备原生项，PIDL 跨进程传递；Pulse 自有的不支持操作禁用。关闭首个窗口后重绑通知，最后窗口退出才关闭设备注册表。 |
| 云盘状态 | 同步根发现与缓存、状态列与悬停说明、保留/释放命令、显式下载后预览。占位判断统一包含 OFFLINE 与两类 recall 标志，已 pinned 但未下载的文件仍受保护。下载按窗口隔离，完成结果核对标签、代次、选择和路径；切换选择后不抢开预览。追补统一扩展路径与普通路径去重，本机两个 OneDrive 账户经后台刷新后只保留两项，见后续性能记录。 |
| 备份与迁移 | 关于与诊断页导出、导入、恢复；导入先暂存，下次主实例加载配置前自动备份并应用。失败时回滚已替换文件，拒绝损坏数据、重复文件名及白名单外路径。兼容本地 preferences、网络索引配置和二进制内容索引配置，保留本机集成及窗口状态。网络代理识别外部导入的配置变化，不把旧根列表写回。 |
| 日期 | 按用户地区的一周起点区分本周与上周；新增日期样本参与列宽计算，包含中英文资源。 |
| Alt 拖放快捷方式 | Alt、Ctrl+Shift 使用 LINK，保留原件；普通 Ctrl/Shift 行为与临时来源保护保持。后台创建、同名编号、并发原子预留、错误反馈和关闭邮箱；长输出路径使用 Shell stream 写入，避免系统 Save 返回成功但生成空文件。 |

手机目录列举进入 provider 工作队列，不占本地目录专用队列。侧栏刷新保留已发现网络项并重新填入云盘缓存。设置任务随异步退出线程收尾，处理窗口关闭后的完成锁释放。

迁移包含可迁移设置及个人列表。窗口位置、会话、托盘暂存内容、撤销历史、最近访问记录、缓存、索引数据和本机 Shell/启动集成不随包迁移；导入保留目标机器的这些状态。

## 七项检查结果（含补充两项）

| 检查项 | 结论 |
| --- | --- |
| 首帧优先、延迟初始化、体积优化 | 已补首帧后六步初始化、早期操作按需初始化、实际导入系统 DLL 延迟加载，以及 Release 函数/数据节裁剪和跨模块优化。冷启动和首个 present 耗时仍需实机测量。 |
| 空闲按需唤醒 | 已补静止五秒后 500 ms、隐藏 1000 ms 的维护频率，以及目录/文件夹大小结果的合并消息唤醒；保留独立 deadline。悬停重绘按用户要求排除。 |
| 菜单、侧栏、标签、快速预览动画 | 按用户决定不移植上游动画，沿用本地各区域现有共用控件与效果；上游效果差异不再列作缺失。 |
| 回收失败不静默永久删除 | 已有本地独立回收 API；明确拒绝后重新确认永久删除，无自动执行永久删除分支。本次为源码审查。 |
| SMB 大量文件复制、删除性能 | 已补网络四路复制、最多八路永久删除、单目录有界删除批次、批量目标名称探测；冲突、提交、源删除与撤销仍走本地对象保护流程。尚未做真实 SMB 吞吐验收。 |
| 缩略图提速 | 已补最多四个独立 host、系统缓存与相机内嵌预览优先、有界下一屏预取、带文件身份检查的持久缓存；可见/详情请求优先，云占位不预读。 |
| 复制、移动、删除提速及大文件进度 | 已补按存储类型限制并发、大文件与小文件并行、聚合实时进度及适用时的无缓冲复制；保留同盘改名。哈希校验与冲突回退串行。“暂停立即生效”及即时反馈排除。 |

七项初始差异保留在 [性能与回收审查](performance-audit.md)。后续适配、定向测试和验收边界合并见 [性能适配记录](../performance-adaptation-20261028/README.md)。下方 251 项为前一轮六项功能的验证结果；性能实现与性能实测分别记录，不能据此宣称真实共享、硬盘或冷启动达到某个加速比。

## 实际构建与测试

MSVC x64、Ninja、Release，构建目录 `build-release`。仅构建受影响产品、必要依赖及定向测试，没有运行全量构建脚本或完整 selftest。`Pulse.exe`、`Pulse.Index.exe`、`Pulse.Shell.exe`、`Pulse.Preview.exe` 均已构建。

最终增量构建见 [产品与界面验证](build-verified.log)、[界面夹具](build-ui-fixture-final.log)、[拖放兼容](build-drag-compat.log)；Shell/预览和产品完整受影响编译见 [集成构建](build-repair3.log)，网络代理更新见 [配置重载构建](build-network-reload.log)。修复后的构建日志未出现新增编译警告。

下表命令均在仓库根目录执行，程序位于 `build-release\`：

| 程序/筛选 | PASS / FAIL | 覆盖与日志 |
| --- | --- | --- |
| `pulse_recent_day_label_test.exe` | 10 / 0 | [本周/上周、周起点、未来与旧日期](recent-day.log) |
| `pulse_localization_test.exe --synced-features` | 6 / 0 | [本次新资源、日期格式和拖放提示中英文](localization.log) |
| `pulse_settings_transfer_test.exe` | 66 / 0 | [导出、暂存、备份、恢复、回滚、本地扩展与二进制配置](settings-transfer.log) |
| `pulse_drop_shortcuts_test.exe` | 11 / 0 | [原件保留、目标与工作目录、冲突、并发、失败、长路径](drop-shortcuts.log) |
| `pulse_drag_drop_test.exe --target-session-only` | 8 / 0 | [进入/悬停/离开/最终数据对象与执行效果](drag-session.log) |
| `pulse_network_locations_feature_test.exe` | 20 / 0 | [地址解析、历史持久化、去重、发现结果合并、取消、本地快捷方式](network-locations.log) |
| `pulse_network_config_reload_test.exe` | 6 / 0 | [生产 NetworkIndex 配置重载、损坏保护、恢复与不写回旧根](network-config-reload.log) |
| `pulse_cloud_features_test.exe` | 20 / 0 | [占位/状态、列宽、pin 属性、后台探测、显式读取、取消、双窗口下载隔离](cloud-features.log) |
| `pulse_device_source_test.exe` | 39 / 0 | [路径、模拟设备、批次列举、锁定/只读/断开、取消、STA 与超时](device-source.log) |
| `pulse_feature_sync_application_test.exe` | 65 / 0 | [生产绘制/命中、云盘菜单与列边界、Alt/修饰键、网络页和手机展示](application-ui.log) |

合计 **251 项最终定向断言通过、0 失败**。重复运行和早期日志不重复计数。配置重载测试直接调用生产索引对象，不启动网络扫描线程或连接真实共享。

早期编译错误（SDK include、本地 JSON API、误用结果变量）已修复，保留 `build-initial.log`、`build-repair.log`、`build-repair2.log`。界面夹具最初漏入产品运行时源/资源的链接问题已修复。网络页初次 4 项断言失败保留在 [初次网络页检查](application-ui-network-initial.log)：内部 UNC 路径带扩展前缀，已将显示路径改为普通 UNC，并按路径等价关系验证入口；没有删除断言。另修正了截图夹具手动切页时未清空标题/提示的状态，正式导航原本已有清理。

## 视觉检查

生成并检查 16 张生产渲染器离屏截图：浅色/深色，100% 常规窗口与 150% 窄窗口，覆盖云盘列、迁移卡片、网络位置页和手机入口。迁移按钮会在窄窗纵向排列，绘制与命中一致；状态列与日期等原有列没有新增重叠。界面夹具数据为合成数据。

- [浅色云盘](screenshots/light-100-cloud.png)、[深色窄窗云盘](screenshots/dark-150-cloud.png)
- [浅色迁移卡片](screenshots/light-100-settings-transfer.png)、[深色窄窗迁移卡片](screenshots/dark-150-settings-transfer.png)
- [浅色网络位置](screenshots/light-100-network-locations.png)、[深色窄窗网络位置](screenshots/dark-150-network-locations.png)
- [浅色手机](screenshots/light-100-phone.png)、[深色窄窗手机](screenshots/dark-150-phone.png)

## 尚未验收的边界

- 环境未检测到真实手机：MTP 驱动、手机授权、真实复制/新建文件夹及设备原生菜单未做真机验收。设备源测试和手机截图使用独立模拟数据。
- 没有连接真实 NAS/SMB 或启动 LAN 发现进行验收；连接服务器弹窗没有自动交互/截图验收，Windows 认证沿用既有实现。网络吞吐与 CPU/GPU/启动性能没有测量。
- 没有对真实 OneDrive 等 provider 执行保留、释放或下载。云盘测试使用属性模型、注入的状态查询和独立本地文件；覆盖下载器双窗口隔离，不代表 provider 全路径验收。
- 本机 Shell 拒绝部分超过 MAX_PATH 的快捷方式目标路径时，显示错误并保留原件；长输出目录已通过有效 Shell stream 校验，不把空快捷方式视为成功。
- 设置核心迁移与故障恢复使用独立配置目录；未导入、恢复或重启用户实际设置。未进行真实设置文件选择器操作。
- 未替换安装目录、打包、发布或推送。已安装程序不会因本次源码与构建更新自动变化。
