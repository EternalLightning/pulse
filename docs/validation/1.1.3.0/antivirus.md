# Pulse.Shell.exe 告警定位与修复

日期：2026-10-16。本记录区分能够复现的构建差异与无法观察的杀毒内部判定。

## 复现与范围

用户报告的文件 MD5 为 `B12B896EFC5F2BB80BC485371B041423`，与本次本地构建的 Shell 相同。用本机 Kaspersky 21.26 的扫描器复现了 `not-a-virus:VHO:AdWare.Win32.ShouQu.gen`；更新前安装备份扫描通过。原始结果见 [基线扫描](shell-av-baseline.txt)。

本次 `src/shell_host/` 主体源码未变化，Shell 的导入函数集合没有增减。新增的多窗口恢复日志状态位于公共 `OpsManager`，该类型也被 Shell 文件操作工作进程构造。此进程直接执行请求，不配置应用窗口的恢复日志。

以 `485e434` 为旧代码基线，在独立目录使用当前编译/链接选项重新生成四组程序。各组均使用 1.1.3.0 版本资源，保留正常嵌入清单。结果见 [因素对照扫描](shell-av-factors.txt)：

| 构建 | 扫描结果 |
| --- | --- |
| 当前代码、本地化和资源，重新链接 | 同一条检测 |
| 旧代码、旧本地化及旧文案，仅使用新版本资源 | OK |
| 当前代码、旧本地化库、当前资源 | 同一条检测 |
| 旧代码、当前本地化库和资源 | OK |

当前代码的重新链接对照物与原触发文件的差异仅为四个时间戳字节，代码、数据、资源节哈希相同，也仍被检测。版本号和本地化资源不足以解释告警；对照将触发范围缩小到本次公共操作管理器的恢复日志状态及其生成代码。无法据此说明卡巴斯基私有规则匹配了哪条指令，也不把静态扫描结果等同于所有运行行为的安全证明。

## 实现修正

`OpsManager` 原来直接持有多日志路径、来源映射和未知记录容器，每个 Shell 工作进程也会构造、析构它们。现在封装为私有 `JournalState`，只有调用 `SetJournalPath` / `SetJournalPaths` 时才创建。未配置日志时持久化直接返回，丢弃恢复记录也不分配状态。

应用窗口继续使用相同的恢复规则，包括独立日志、遗留日志汇集、重复及近溢出序号重映射、未知记录保留。没有改变权限检查、IPC 校验或文件身份保护。

这是针对工作进程职责的实现调整。没有改程序名、混淆、加壳、关闭防护、添加排除项或恢复被报出的二进制文件，也未向第三方提交程序样本。

## 修复后的实际验证

| 对象 | 时间 | 结果 |
| --- | --- | --- |
| `build/Pulse.Shell.exe` | 22:53 | 2 个扫描对象 OK，0 检测、0 错误 |
| 最终 40 文件暂存载荷 | 22:55 | 71 个扫描对象 OK，0 检测、0 跳过、0 损坏、0 错误 |
| 安装路径中的新 `Pulse.exe` / `Pulse.Shell.exe` | 22:56 | 2 个扫描对象 OK，0 检测、0 错误 |

提交的扫描报告副本仅清理行尾空白，本地原始输出保留在 `bench_data/release-1.1.3.0/`。对象数包含引擎解析出的内部对象，不能直接当成文件数。扫描库均为 `2026-10-16 09:29:00`。原始报告分别为 [Shell](shell-av-lazy.txt)、[完整载荷](payload-av-final.txt)、[安装文件](installed-av-final.txt)。

使用本机扫描器，以报告模式检查实际文件。例如：

```powershell
& 'C:\Program Files (x86)\Kaspersky Lab\Kaspersky 21.26\avp.com' SCAN `
    'E:\Projects\pulse\build\Pulse.Shell.exe' /i0 /fa `
    '/RA:E:\Projects\pulse\bench_data\release-1.1.3.0\shell-av-lazy.txt'
```

控制台提示非交互模式可能忽略 action 参数，因此同时检查原始报告：`Action on detect: Report only`、`Try disinfect: No`、`Try delete: No`、`Try delete container: No`。未调整防护配置。

修复后 Shell：

- MD5：`513484C0618DC0C2690AE4E1BAD5136E`
- SHA-256：`DA19BB8EB5C077202DE605105B2A212AB4AAF2DAAAFB19FDF632CF5137909598`

原触发 Shell 的 SHA-256：`91653CD221D91720CC1A87E3544AD9C6A92D623CAB4CE0628F352D2C5482C840`。

日志回归为 [14 PASS](operation-journals.log)，新增检查无日志 Shell 工作进程不分配状态，以及应用窗口配置后创建状态。Shell 操作适配器为 [34 PASS](elevation-adapter-lazy.log)，运行真实操作引擎和同权限传输，覆盖复制、移动、撤销、恢复记录、身份替换拒绝及删除确认；未执行原生 UAC 交互用例。

最终载荷正常启动、渲染及退出，截图已查看；复制后的 40 文件哈希复核一致。安装版主窗口仍保持未启动。结论是此次可复现告警在上述实际扫描中已消失，不能保证今后的引擎规则或未验证的运行场景永不告警。
