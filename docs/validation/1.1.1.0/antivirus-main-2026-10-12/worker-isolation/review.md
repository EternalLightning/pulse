# Pulse 管理员 worker 隔离与卡巴斯基复扫

2026-10-12。本次真实构建的 Pulse.exe 与 Pulse.Shell.exe 在同一轮本机卡巴斯基扫描中均未检出，原始 MD5 为 `0CEB8066D10A147C368FEEE9697F22CD` 的样本仍检出 `VHO:Trojan.Win32.BypassUAC.gen`。这是一组当前构建、当前本机规则下的对照结果，不是厂商误报认定，也不能保证未来规则或其他机器上的结果。

## Git 与源码依据

- 最后保留的未检出主程序版本为 1.1.0.1，对应 Git 基线 `185cb83`。管理员文件操作 adapter、transport 和主程序 worker 入口首次出现在 `3e5f1517c36dbf46b2fff6f7631571616089c2e8`。
- 原实现用 `ShellExecuteExW` 的 `runas` 申请标准 UAC 授权；令牌查询用于核验已获得的权限。本次定向审查未发现静默 UAC 绕过，检测名称不能替代对具体实现的判断。
- 默认文件管理器的 `DelegateExecute` 写入属于用户目录/驱动器/Win+E/ThisPC 打开关联；索引中的 `DuplicateTokenEx` 用于捕获管道客户端模拟令牌。本次没有修改这些功能。
- 对照历史范围较大，扫描规则没有公开；不能从两次产物结果推断某个 API、字符串或函数一定就是触发原因。

## 实际修改

- 主程序保持 `asInvoker`，管理员文件操作由既有同目录 `Pulse.Shell.exe` 承载。主程序旧 worker 参数在 UI 启动前返回 `ERROR_NOT_SUPPORTED`。
- 父进程选择 Shell 辅助程序；缺失时报告辅助程序路径，不回退到主程序。
- 生产通道只接受主程序与同目录 Shell；保留标准 UAC、双方 PID/创建时间/用户/路径/管道 ACL 核验、取消、部分完成、未知结果与禁止自动回放机制。
- 只为生产 Shell 加入操作依赖及字符串资源；其他 Shell 测试入口不引用管理员 adapter。版本没有改变，已有并行解压/UI 修改保留。

这种分离有明确的最小权限收益，符合 [Microsoft 的主程序与管理员 helper 分离建议](https://learn.microsoft.com/en-us/windows/win32/secbp/running-with-administrator-privileges)。没有删减管理员文件操作、撤销、默认文件管理器等用户功能，也没有加入混淆、壳或降低 PE 安全保护的设置。

## 本机杀毒对照

Kaspersky 21.26，扫描显示病毒库时间 `2026-10-12 09:25:00`。扫描指定 `/i0 /fa /iChecker=off /iSwift=off`；报告确认 `Report only`、不尝试修复或删除，没有改变全局保护配置。

| 文件 | MD5 | 2026-10-12 19:10 同轮结果 |
|---|---|---|
| 新 Pulse.exe | A1C192E592327E86F1297A1E7A32EA6F | 未检出 |
| 新 Pulse.Shell.exe | 77C561669659D44098F9BA8EA8026B51 | 未检出 |
| 原始 Pulse.exe | 0CEB8066D10A147C368FEEE9697F22CD | 仍检出 BypassUAC |

对应 SHA-256：

- 主程序：`AF45578D96F37089D2817326081F635BD233DCF258C800BC8847322C7970E93A`。
- Shell：`EE9E688FECD9866A89CEB1F35F121E3AC6504CA175B25ABE0090F060C899473A`。
- 原始样本：`B4BA5A3340CA6A4BA89B5ED15A5878CBB781CACA4B2D5DF318446B64D23ADF84`。

日志共计 4 对象（含一个嵌入对象），3 OK、1 detected、0 errors。主程序和 Shell 各自的条目均为 OK；不能把混合对照总计写成 0 detected。

## 定向验证

- 显式构建主程序、Shell、elevation adapter 与 transport 四个目标及 CMake 关联的必要生产依赖；构建成功。首轮第三方 SQLite 有既有 `/W4` 被 `/W0` 覆盖警告，配置与 HEAD 相同；最终增量日志没有新增警告或错误。
- 内容和 SHA-256 一致的临时副本，在确认 RID 8192 的普通 Windows 环境运行：transport 20 PASS / 0 FAIL，adapter 34 PASS / 0 FAIL，退出码均为 0。覆盖复制、移动、重命名、撤销、删除/回收、冲突桥接、取消、未知完成结果、协议边界、身份限制和缺失 helper 时的无修改失败。
- 初次 RID 4096 沙箱运行的失败日志保留；随后实际执行和结果记录见 `medium-results.json`，没有删除或弱化失败用例。
- 真实生产程序的无效 worker 参数检查：主程序退出 50，Shell 退出 87，符合预期，日志见 `production-worker-boundary.json`。
- 从真实两份 EXE 提取清单，两者均为 `asInvoker` / `uiAccess=false`。两者保留 CFG、ASLR、高地址熵、NX；依赖检查未见新增未打包的 MSVC 运行库。
- 原生 UAC 验证已运行，用户在系统提示中允许授权：6 PASS / 0 FAIL，退出 0，`RESULT=UAC_COMPLETED`。真实生产 Shell 成功把测试内容复制到隔离的 ACL 受保护目录，父进程只导入一次完成结果，恢复原 ACL 并清理文件。测试父进程为隔离目录中改名为 Pulse.exe 的测试程序，符合生产的同目录主程序身份限制；它不等于从实际 Pulse UI 点击权限提示的全链路验证。记录见 `native-uac.log` 和 `native-uac-verification.json`。本轮没有再次运行用户拒绝 UAC 的原生分支。
- 本次没有运行完整 selftest 或所有历史测试，没有覆盖真实 UNC/云盘、所有 UI 模式或安装目录部署。

## 外部复核状态

用户先授权提交原 EXE 与检测日志，随后要求“先不提交了”。已停止进一步外发。之前上传请求发生文件访问/传输错误，没有上传成功响应、复核编号或厂商分析结论；无法确认服务器是否收到部分请求数据。提供的邮箱没有用于提交复核。原申请草稿保留，未经新的明确指示不会继续提交。

[Kaspersky 的官方误报处理流程](https://support.kaspersky.com/kaspersky-for-windows/21.27/troubleshooting/other/1870)仍可作为后续正规复核途径；[签名与 Allowlist 建议](https://support.kaspersky.com/kes-for-windows/14.2/settings/protection/15957)可减少新版误报风险，但不能保证零检出。本次没有数字签名或申请 Allowlist。

## 使用边界

本次新文件位于 `build/Pulse.exe` 和 `build/Pulse.Shell.exe`，更新时必须使用配套的两份程序，并保留同目录其余运行依赖。本次没有覆盖安装目录、测试目录中的原样本或已经发布的便携 ZIP。

已生成独立的配套更新包 `dist/Pulse-1.1.1.0-antivirus-update-20261012.zip`，3,976,443 字节，SHA-256 为 `D2CB5C28F767C668A8808420E4C252FF94AA55F9684AEDF6BB32C66E77456BEC`。包含两份程序及 UPDATE.txt，直接打开 ZIP 逐项核验内容 SHA-256 与实测构建一致，记录见 `update-package.json`。它用于更新已有完整目录，尚未部署，不替代既有正式便携包的全部内容。

本目录保留原始扫描、输入哈希、构建与定向测试日志、生产清单及依赖检查、外部请求失败状态。
