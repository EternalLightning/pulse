# 上游行为适配定向验证（2026-10-25）

范围：多窗口标签/窗格转移、虚拟文件粘贴回退、SMB 认证、网络发现提示及驱动器改名。保留本地共享服务、窗口生命周期、操作中心和已有未提交修改；未直接合并上游历史。行为参考为上游 `c4fe01ed82fe091f495f531972a854aa32caac2b`。

## 实现

- 转移原有窗格对象，只接管传入窗格的后台请求；目标已有窗格不重新导航。保留可见快照、选择、筛选和滚动，跨 DPI 同步调整当前及历史视口。文件名搜索重新订阅时覆盖已加载页；内容搜索接管保持旧结果，查询结果未变时沿用原结果存储。
- 按真实桌面 Z 顺序确定目标，其他应用遮挡时不穿透；桌面宿主允许拖出新窗口。Esc 取消标签/窗格拖动，并通过本线程键盘和消息钩子识别标题栏原生移动取消。
- 实际路径读取失败或为空时允许回退至虚拟 Shell 格式；保留有效文件路径/托盘规则、剪贴板序号、后台 STA 和结果邮箱保护。
- 仅当前可见前台标签的 UNC 权限/认证失败触发 Windows 登录，不自行存储密码。按共享去重，结果绑定窗口、标签身份与指针、视图、列举代次和路径；后台轮询避让认证等待。登录成功后的重新列举不自动开启下一轮凭据提示。
- 只有活动防火墙配置的网络发现状态全部明确关闭，才在主页网络区提示并提供打开系统设置的入口；未知、混合或查询失败不显示关闭提示。
- 固定盘保留 `SetVolumeLabelW`；映射盘通过后台 Shell `SetNameOf` 修改显示别名，成功后回读实际名称。主页、“此电脑”和侧栏复用映射盘名称；不修改服务端卷标，不生成普通文件重命名撤销或错误的卷标提权请求。

## 实际验证

构建目录 `build_release_clean`，MSVC x64 + Ninja。构建受影响产品及下表测试目标成功，无新增编译警告。见 [构建日志](build.log)、[最后构建](build-final.log)。未运行完整 selftest、全量测试或发布脚本。

| 检查 | 命令/筛选 | 结果 | 日志 |
|---|---|---|---|
| 多窗口转移 | `pulse_upstream_adaptation_test.exe --transfer-only` | 18 PASS / 0 FAIL | [日志](window-transfer.log) |
| 搜索接管结果 | `pulse_upstream_adaptation_test.exe --search-rebind-only` | 5 PASS / 0 FAIL | [日志](search-rebind.log) |
| 网络请求模型 | `pulse_network_access_test.exe` | 16 PASS / 0 FAIL | [日志](network-model.log) |
| 网络应用接线 | `pulse_upstream_adaptation_test.exe --network-only` | 18 PASS / 0 FAIL | [日志](network-application.log) |
| 剪贴板来源与可用性 | `pulse_clipboard_availability_test.exe` | 40 PASS / 0 FAIL | [日志](clipboard.log) |
| 驱动器分流、失败与取消 | `pulse_ops_test.exe --filter drives` | 103 PASS / 0 FAIL | [日志](drive-rename.log) |
| 主页布局、名称和真实 Z 盘只读检查 | `pulse_home_layout_test.exe` | 沙箱内147、沙箱外149 PASS / 0 FAIL | [沙箱](home-layout.log)、[真实Z](home-layout-real-z.log) |
| 主页提示绘制和命中 | `pulse_upstream_adaptation_test.exe --home-notice-only` | 8 PASS / 0 FAIL | [日志](home-notice.log) |

共357项最终定向断言（主页采用覆盖真实Z盘的149项结果，未重复累加沙箱用例）。Z盘检查只读取现有rclone挂载的Shell显示名和容量，不执行改名、重连或文件写入。

初次标题栏Esc队列检查有1项失败，保留在 [初次日志](window-transfer-initial.log)；补充消息队列取消识别后18项通过，未删除或放宽断言。初次隐藏窗口截图因测试夹具调用Present而为空白，最终以隐藏截图模式重新捕获；截图夹具也补齐了正式窗口初始化的主题色。

## 视觉检查

提示使用模拟的明确Disabled缓存状态，未更改系统防火墙。截图夹具没有载入用户目录和卷卡片，因此空列表不代表用户实际主页状态。已检查浅色/深色、100%/150%、常规/窄窗口提示及链接，没有发现新增提示的裁切和重叠。

- [浅色100%](screenshots/light-100.png)
- [深色100%](screenshots/dark-100.png)
- [浅色150%窄窗口](screenshots/light-150.png)
- [深色150%窄窗口](screenshots/dark-150.png)

## 验证边界

- 多窗口测试使用真实离屏窗口、Z顺序及生产消息处理，不等同物理鼠标拖动或跨显示器DPI验收。
- SMB结果通过隔离邮箱注入；没有向真实服务器提交凭据，也没有验收真实Windows登录弹窗。进入同步provider调用后的凭据窗口不能可靠强制终止；取消/切走/关闭会撤销结果接收，不阻塞退出。
- 映射盘改名使用fake backend验证分流和结果语义，没有修改真实Z盘别名、卷标、注册表或网络连接。
- 未在真实Outlook/RDP会话中验收虚拟文件，也未将Shell粘贴接入Pulse操作中心的进度、暂停或撤销；关闭结果邮箱不等于中止已执行的Shell传输。
- 未替换已安装程序，未发布或推送。
