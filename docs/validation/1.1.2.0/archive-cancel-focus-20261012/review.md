# 解压取消后恢复主界面焦点

## 原因与修改

解压共用 FileOperationWindow，但调用方会禁用主窗口，关闭路径此前却按非模态方式调用 HideComposedDialog。活动弹窗消失时主窗口仍禁用，Windows 可以激活下面的窗口；调用方随后只 EnableWindow，没有恢复激活与键盘焦点。

在 FileOperationWindowOptions 中增加默认关闭的 modal_owner 标志，仅由确实禁用原先可用 owner 的解压调用方启用。Hide / Destroy 将该 owner 传给现有 HideComposedDialog，复用统一生命周期规则：活动弹窗关闭前先启用 owner，并恢复激活与焦点，再隐藏和销毁弹窗。原本禁用的 owner 不被擅自启用；普通复制/移动窗口默认行为保留。后台完成时沿用公共方法的活动窗口判定，不强制抢前台。

取消按钮仍只通知取消，等解压线程结束才关闭窗口并释放主界面。没有修改解压引擎、文件清理、密码处理、取消结果或 UI 布局。

## 定向验证与限制

- 在 pulse_archive_dialog_test 增加独立 `--focus-only`，使用真实生产进度窗口和测试 owner / 无关窗口。覆盖取消按钮、Esc、标题关闭、正常结束、错误窗口关闭、切换到无关窗口后结束。
- 修复前原检查 33 PASS / 10 FAIL，五种活动弹窗关闭路径均观测到无关底层窗口被激活，见 before.log。原检查还把未获 Windows 前台激活权限的测试窗口当作前台窗口；这个前置条件不成立，不能据此判断桌面前台恢复。最初修复后及普通权限副本仍有 6 个这类断言失败，完整保留 after.log、after-initial-diagnostic.log、focus-medium.log。
- 最终检查按实际初始前台窗口验证：测试取得前台时要求关闭后 owner 在前台；未取得前台时明确输出 LIMIT，并检查外部前台不被抢走。激活状态、键盘焦点、无关窗口激活计数及取消返回值始终检查，未删除这些回归断言。最终 49 PASS / 0 FAIL，退出 0；本次六种模式均受前台激活限制，因此真实桌面前台切换仍需手动确认，见 focus-final.log。普通权限副本哈希一致，路径见 fixture-path.txt。
- 直接依赖的 `--progress-only`：38 PASS / 0 FAIL，退出 0。验证模态 owner、进度发布、取消后等待工作线程退出、WM_QUIT 保留。未运行完整历史自测或解压引擎测试。
- 仅构建 pulse 与 pulse_archive_dialog_test，最终构建无编译警告/错误。最初新增测试的未使用参数警告已修正，最终测试构建日志为 focus-build-final.log。
- 卡巴斯基于 2026-10-12 22:34:31–22:34:33 对新 Pulse.exe 复扫，1 对象 OK，检测/可疑/跳过/错误均 0，退出 0。沙箱命令初次返回 -3 且未产生报告，普通系统权限复扫成功，见 kaspersky.log；防护配置未修改。

## 部署与手动验收

用户此前授权更新安装目录，执行前确认 Pulse 与 Pulse.Shell 均未运行。仅备份并替换 `D:\Applications\Pulse\Pulse.exe`，版本仍为 1.1.2.0；安装目录全部 40 个载荷文件哈希通过，PulseIndex 服务保持 Running，未修改服务或启动主界面。准确时间、备份路径、新旧哈希见 [deployment.json](deployment.json)，执行脚本为 [deploy.ps1](deploy.ps1)。便携 ZIP 未重新打包。

手动打开 Pulse，启动较大压缩包解压，分别点击取消、按 Esc、点击标题关闭；待取消清理结束后应回到主界面，键盘操作应作用于 Pulse，下面的其他程序不应被激活。解压过程中主动切换到其他程序并等待结束时，应保持用户正在使用的窗口。
