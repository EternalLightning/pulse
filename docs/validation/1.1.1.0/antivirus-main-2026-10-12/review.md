# Pulse 主程序告警复核（2026-10-12）

后续更新：管理员文件操作迁移到既有 Pulse.Shell.exe 后，2026-10-12 19:10 的同轮本机扫描中，新主程序与新 Shell 均未检出，原始样本仍检出。两组定向测试 54 PASS / 0 FAIL，随后真实 UAC 隔离复制验证 6 PASS / 0 FAIL。下文保留先前排查阶段的原始结论；当前新构建结果与边界见 [worker-isolation/review.md](worker-isolation/review.md)。用户随后要求停止外部提交，已停止；没有上传成功或厂商受理回执。

结论：本机扫描可独立复现 `VHO:Trojan.Win32.BypassUAC.gen`。更新病毒库、补充标准运行权限声明均未消除检测。定向源码审查未发现 UAC 绕过机制，但没有厂商误报结论，也没有完整二进制安全审计，不能确认文件安全或保证不再告警。当前停止将该载荷部署到安装目录。

## 与前次记录对照

| 对象 | MD5 | 结果 |
|---|---|---|
| 2026-10-11 首次 1.1.1.0 Pulse.Preview.exe | BD23B0E1800B2E481738BEF849E90248 | VHO:Trojan-PSW.Win32.Convagent.gen |
| 前次 ACK 资源状态修正后的预览程序 | A4D871DA4C5CFB1C916388607E786048 | 当时本机扫描未检出；无厂商误报结论 |
| 保留的 1.1.0.1 Pulse.exe | 4442128433779292AD1779C1DBF24C5E | 本次扫描未检出 |
| 本次通过用户测试的 Pulse.exe | 0CEB8066D10A147C368FEEE9697F22CD | 本次扫描检出 BypassUAC，与用户事件一致 |
| 当前重新构建的 Pulse.Preview.exe | D615FF841153F016E8E39FDE4C4AD4D8 | 本次扫描未检出；不同于前次构建哈希 |
| 补充运行权限声明后的 Pulse.exe | 356942EE85F3FB10A664C0EAB583B4AF | 仍检出 BypassUAC |

历史依据为上级目录 `antivirus-review.md`、`antivirus-detection.json`、`preview-view-state-verification.json`，没有把前次代码修正与检测规则之间的因果关系当作已证事实。

## 本次实际检查

- 构建目录与 E:\Projects\TestPrograms\Pulse 的原主程序长度均为 7,250,944 字节，MD5 相同；SHA-256 为 B4BA5A3340CA6A4BA89B5ED15A5878CBB781CACA4B2D5DF318446B64D23ADF84，与部署清单一致。签名为 NotSigned。没有证据表明复制时改变了内容。
- Kaspersky 21.26，第一次扫描显示病毒库 2026-10-12 06:37:00。三个指定文件扫描：2 OK、1 detected。没有执行这些样本。
- 用产品已配置的来源正常更新病毒库，更新退出 0。更新后扫描显示病毒库 2026-10-12 09:25:00；原主程序仍检出同一名称。
- 扫描指定 `/i0 /fa /iChecker=off /iSwift=off`，缓存优化仅对这些扫描关闭。CLI 提示非交互模式忽略动作参数；实际报告 Settings 为 Report only，Try disinfect/delete/container = No。没有关闭保护、添加排除或改动全局扫描配置。
- 编译器 cl.exe、链接器 link.exe、资源编译器 rc.exe 的签名均为 Valid，签名者 Microsoft Corporation；哈希与前次记录相同。此项不能证明完整构建链和产物未受污染。

## 定向源码结论

- `src/ops/operation_elevation_adapter.cpp:1378` 获取主程序路径；`src/ops/operation_elevation_transport.cpp:337` 使用 ShellExecuteExW + runas 请求一次性文件操作 worker。
- `src/ops/operation_elevation_transport.cpp:549` 要求 worker 已经处于 elevated/high integrity，并核验同用户、父 PID/创建时间、预期程序路径、管道双方 PID。管道名称随机、ACL 受限、禁止远程客户端；连接使用 SECURITY_IDENTIFICATION。
- `src/shell_host/shell_interaction_worker.cpp:48` 的另一路管理员启动同样使用 runas；索引服务配置通过 `src/index/index_client.cpp:514` 调用它。
- `src/app/default_file_manager.cpp:61` 的空 DelegateExecute 写入位于 Directory/Drive/Win+E/ThisPC 默认打开关联。该定向范围没有发现 fodhelper、computerdefaults、ms-settings/mscfile 提权路径、令牌复制/特权修改、进程注入或提升 COM moniker。
- 相对 HEAD 3e5f151，上述提权路径、权限提示、默认关联、app_main、prefs 文件没有本次 dirty diff。解压变更仍使用普通 CreateProcessW，仅增加进度管道等解压逻辑。
- 以上仅为指定源码范围审查，不是完整安全认证。检测名称和这些 API/字段的存在都不足以证明规则触发原因。

## 规范修正与验证

`cmake/pulse.manifest.in` 增加 `requestedExecutionLevel level="asInvoker" uiAccess="false"`，明确主程序按调用者权限运行；需要管理员权限的既有 runas 路径保留。此前已嵌入 manifest，但未明确 requestedExecutionLevel。

仅构建目标 pulse，成功，无新增警告。mt.exe 从真实 Pulse.exe 的资源 #1 提取并校验清单，XML 校验确认 asInvoker 和 uiAccess=false。没有为本项清单修正运行完整功能回归或执行被告警主程序。

修正后 SHA-256：224AA0010FC51DA4770E344C7C783C5F228D5E7578B45E149C75A7D152640DD9。新旧两个哈希均被检出，因此不能把本项规范修正描述为报毒修复。测试目录与便携包保留原用户测试载荷；没有用新产物覆盖它们，也没有更新其测试通过结论。

## 部署现状及后续边界

本次部署失败记录为 `bench_data/deploy-tested-pulse-result.json`：目标 Pulse.exe 被占用，复制未完成。当前 D:\Applications\Pulse\Pulse.exe 不存在；仅凭此状态不能确认隔离动作。PulseIndex 服务已经恢复 Running，路径仍是该安装目录的 Pulse.Index.exe --service。不能宣称整套安装已更新或可正常启动。

当前不能保证通过代码改动消除告警。应针对原被告警样本及日志申请厂商复核；正式代码签名可作为长期减少误报的措施，但不能保证不会检出。没有上传样本、源码，或提交任何外部复核请求。`vendor-review-draft.md` 仅是本地草稿。

官方依据（2026-10-12 查阅）：

- [Microsoft：ShellExecuteEx 的 runas 触发 UAC 同意/凭据提示，NO_UI 不抑制安全提示](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shellexecuteinfow)
- [Microsoft：应用权限清单和最小权限运行](https://learn.microsoft.com/en-us/windows/win32/secbp/running-with-administrator-privileges)
- [Kaspersky：疑似误报的更新、复核和资料收集流程](https://support.kaspersky.com/kaspersky-for-windows/21.27/troubleshooting/other/1870)
- [Kaspersky：对私有软件进行数字签名以减少新版误报](https://support.kaspersky.com/kes-for-windows/14.2/settings/protection/15957)

## 本目录证据

- comparison-inputs.json：原始对照文件元数据和哈希。
- kaspersky-comparison.log、console.txt、exit.txt：三个文件的首次扫描。
- kaspersky-update.log、console.txt、exit.txt：病毒库正常更新。
- kaspersky-original-after-update.log、console.txt、exit.txt：更新后原哈希仍检出。
- manifest-build-input.json、kaspersky-manifest-build.log、console.txt、exit.txt：规范修正后仍检出。
- embedded-manifest.xml：真实新 EXE 中的清单。
- build-tool-signatures.json：本次核对的工具签名和哈希。

## 继续排查：模块对照与干净构建

用户要求继续查资料及寻找解决方案后，补充以下实际检查：

- 构建现有 pulse_default_file_manager_test、pulse_operation_elevation_transport_test，未修改其源码，仅用作静态扫描对象，没有运行功能测试。
- 上述两个模块测试程序和当前 Pulse.Shell.exe 均未检出，主程序仍检出。日志处理 7 对象（包含嵌入对象），6 OK、1 detected。单个模块未检出并不能证明其组合不是触发条件，测试宏/链接保留范围也与产品不同；不能据此定位某个字符串或 API 为根因。
- 在独立 build/antivirus-clean 中，用同一 MSVC、Release 配置、PULSE_WITH_SELFTEST=OFF 和原 build ID 20261010T093402Z-3E58FAE1 从源码重新构建 pulse 及必要生产依赖，没有复用原构建的对象文件。初次沙箱配置出现 C1902 程序数据库管理器不匹配，原配置日志保留；在普通 Windows 权限环境使用相同配置重试，编译器探测和构建通过。第三方 SQLite 出现既有 /W4 被 /W0 覆盖警告，相关配置与 HEAD 相同，本次未改动。
- 干净构建 MD5 5C77CB6C6C2D218043CF0DE90212A1C1，SHA-256 A5C8A85331271129943667BEA29937E4B5DAD5C7D739DA22439094422E046E21。在同轮扫描中，原构建和干净构建均被检出相同 BypassUAC 名称，2 detected、0 OK。
- 干净构建的 .text、.data、.pdata、.didat、.fptable、.rsrc、.reloc 原始节哈希与当前 build 主程序一致；.rdata 不同。该结果支持告警不是原目录的陈旧代码对象或增量链接特有现象；并非完整构建链安全认证。
- 补充清单前后 .text 等代码/数据节相同，资源节和 .rdata 不同。这符合本次只改清单并重链接的操作，没有用新功能改动冒充报毒修复。
- 普通权限环境只读检查 CurrentUser/My、LocalMachine/My，包含私钥的 Code Signing EKU 证书数量为 0。未读取或导出私钥。仓库已有 scripts/package_release.ps1 正式签名流程，本次没有证书可用于实际签名。

补充证据：module-scan-inputs.json、kaspersky-module-comparison*.log/txt、clean-build-pe-comparison.json、manifest-pe-comparison.json、kaspersky-clean-build-comparison*.log/txt、clean-build-initial-configure.yaml、clean-build-final-configure.yaml、clean-build-ninja.log、signing-availability.json。具体方案、条件和适用边界见 research-and-options.md。
