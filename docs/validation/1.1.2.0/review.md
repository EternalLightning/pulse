# Pulse 1.1.2.0 打包验证

2026-10-12。版本源 `version.txt` 更新为 1.1.2.0，构建 ID `20261012T113225Z-E52E6BD6`。本轮只更改版本与验证文档；当前工作区已有源码修改保留并编入便携包，包括之前的管理员 worker 隔离以及已有压缩包/界面修改。

## 产物

- `dist/Pulse-1.1.2.0-portable-win-x64.zip`
- 12,053,612 字节。
- SHA-256：`540FC58F525054786C66DE37815D0B99DC47486A29706CB5CDDBC30D9AE30570`。
- 完整载荷 40 个文件，包含五个生产 EXE、运行库、7zip 及许可证。

## 实际执行与结果

通过 `scripts/vcvars.bat` 加载 MSVC 环境，配置 Release、`PULSE_WITH_SELFTEST=OFF`，显式构建 `pulse pulse_index pulse_document pulse_preview pulse_shell`。版本资源影响全部五个生产程序，因此本次构建五个目标；没有运行全量 Release 脚本或完整自测。配置、构建退出码 0，最终日志未发现编译警告或错误，见 `configure.log`、`build.log`、`build-exit.txt`。

执行 `scripts/package_portable.ps1 -BuildDir build`，脚本内部完成正式载荷检查，见 `package.log`。独立打开 ZIP，逐项比较条目与暂存载荷 SHA-256：40/40 一致；没有重复路径、越界路径、测试程序、日志或开发中间产物。五个 EXE 的 FileVersion、ProductVersion 均为 1.1.2.0，OriginalFilename 匹配。见 `package-verification.json`、`payload-manifest.json`。

通过 `verify-pe.bat` 检查五个程序的 PE 依赖并提取主程序/Shell 清单。未发现未附带的 VCRUNTIME/MSVCP/CONCRT 依赖；主程序内嵌清单版本为 1.1.2.0，`asInvoker`、`uiAccess=false`。原始输出保留为 `*-dependents.txt` 和 `*-embedded-manifest.xml`。

卡巴斯基 21.26 于 19:35:32–19:35:36 扫描与 ZIP 逐项一致的整个暂存载荷，病毒库时间 `2026-10-12 09:25:00 (full)`。使用 `/i0 /fa /iChecker=off /iSwift=off`；报告确认只报告、不修复。结果为 72 个扫描对象、72 OK、0 detected、0 suspicious、0 skipped、0 errors，退出码 0。扫描对象数不等于文件数；40 个实际载荷文件均有 OK 条目。扫描后再次核验全部文件哈希未变，生产 EXE/运行库与构建输入一致。见 `kaspersky-payload.log`、`kaspersky-payload-console.txt`、`kaspersky-payload-exit.txt`、`antivirus-verification.json`。

当前扫描结果只适用于本次文件和本机规则，不是厂商误报结论或未来不报毒的保证。没有关闭防护、添加排除项或向外部厂商继续提交文件。

## 保留与验证边界

上一轮五个 1.1.1.0 生产 EXE 已在独立目录保存，保存前后 SHA-256 一致；路径、哈希和打包开始时的工作区状态见 `previous-build.json`。旧包和既有修改未删除。

之前 1.1.1.0 构建的 adapter 34 PASS、transport 20 PASS、原生 UAC 允许分支 6 PASS 属于历史定向结果，见 `../1.1.1.0/antivirus-main-2026-10-12/worker-isolation/review.md`。本次新版本未重跑这些功能用例，也未执行实际 UI 全链路、拒绝 UAC 的原生分支、真实 SMB/RaiDrive 或完整桌面回归。编译及扫描通过不能替代这些功能验收。

打包阶段未覆盖安装目录、未安装索引服务、未生成安装包、未签名或发布。下一步手动验收见 [manual-test-plan.md](manual-test-plan.md)。

## 随后按用户指示复制到程序目录

2026-10-12 20:06，用户要求将程序复制到 `D:\Applications\Pulse`，并在标准 Windows UAC 中允许管理员权限。临时停止该目录已有的 PulseIndex 服务，备份 39 个已有同名文件后复制完整载荷 40 个文件。复制前目标目录不存在 Pulse.exe；本次补齐该文件。备份位于 `E:\Projects\pulse\build\deployment-backups\Pulse-before-1.1.2.0-20261012-200643-c625ac22865149d4877a198d6873510f`。

复制后五个 EXE 均为 1.1.2.0，全部 40 个文件 SHA-256 与已验证载荷一致。索引服务已重新启动，状态 Running，自动启动模式和原服务路径保留。随后独立只读复核全部 40 个目标文件哈希及服务实时状态，再次通过。没有启动主界面，也没有改变用户设置。结果见 `deployment.json`、`deployment-service.json`；执行脚本为 `deploy.ps1` 与 `deploy-with-service.ps1`。

21:00 根据后续测试反馈，再次更新主程序和 Shell，修复速率/剩余时间估算及解压错误窗口。前述完整 ZIP 保留原打包快照；安装目录当前两份 EXE 的新哈希、备份与验证结果见 [反馈修复记录](user-feedback-20261012/review.md)。

21:28 根据 F: 到 E: 的机械硬盘复制反馈，修复 CopyFile2 无字节通知错误上报 0/0 并重置节流的问题，备份并更新主程序与 Shell。真实跨盘对照、定向回归、卡巴斯基扫描及最新部署哈希见 [复制回调修复记录](copy-callback-20261012/review.md)。便携 ZIP 仍为原打包快照。

随后修复解压取消关闭窗口时主界面的焦点恢复顺序，复用公共模态窗口生命周期方法。主程序已再次备份并更新；测试、桌面前台验证限制和最新主程序哈希见 [解压取消焦点记录](archive-cancel-focus-20261012/review.md)。Shell 保留 21:28 版本，便携 ZIP 未重新打包。
