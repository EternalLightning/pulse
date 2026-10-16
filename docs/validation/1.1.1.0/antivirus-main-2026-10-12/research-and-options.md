# Pulse 疑似误报解决方案：资料与实测

后续实测更新：下文第 4 项的管理员 worker 分离已实施，新 Pulse.exe 和 Pulse.Shell.exe 在同轮本机扫描中均未检出，原始样本仍检出；54 项定向夹具及 6 项真实 UAC 隔离复制检查通过。当前结果和限制见 [worker-isolation/review.md](worker-isolation/review.md)。下文是实施前的方案记录，不代表当前仍未找到有效的本地改进。用户要求“先不提交了”，已停止外部复核，未取得受理回执。

2026-10-12。没有找到一项已经在本机消除当前 BypassUAC 告警的代码修复。新增模块对照和同源码干净重建均已完成，详见 review.md 和原始扫描日志。以下区分官方处理方案与尚待验证的工程改造。

## 1. 厂商复核当前样本

Kaspersky 官方指引要求在更新病毒库后，查询 Threat Intelligence Portal；不同意结果时通过 Submit to reanalyze 或客服渠道提供事件和样本复核。该方案直接处理当前误判的检测结果，能否被判定为误报、何时更新规则仍由厂商分析决定。本地 vendor-review-draft.md 已可审阅，尚未提交。

来源：[疑似误报处理指引](https://support.kaspersky.com/kaspersky-for-windows/21.27/troubleshooting/other/1870)。

## 2. 持续发布：开发者 Allowlist 计划

这是厂商维护的合法软件库，区别于在用户电脑上设置防护排除。计划免费，面向开发者/供应商/客户；但申请条件包括有效公司与产品网站、网站上的公司法律地址、准确透明的软件行为等。尚未核对 Pulse 是否满足这些条件，不能替用户填写公司资料或宣称已获得准入。

Kaspersky 官方说明其检测规则会在合法对象库上验证，通过开发者提交的软件扩充该库以减少误报。持续参加该计划更适合应对后续版本反复告警，仍需要准入和实际结果验证。

来源：[Allowlist Program 条件](https://www.kaspersky.com/partners/allowlist-program)、[官方对检测规则及合法对象库的说明](https://www.kaspersky.com/blog/product-updates-best-practices/51817/)。

## 3. 每次发布进行正规代码签名

Kaspersky 官方建议对私有软件进行数字签名，以减少新版本误报。当前 Pulse 程序未签名；仓库 scripts/package_release.ps1 已使用 signtool 对五个自有 EXE 和安装器进行 SHA-256 签名及 HTTPS 时间戳，不需要重新设计整套签名系统。

本机 CurrentUser/My、LocalMachine/My 没有包含私钥的代码签名证书，因此无法立即执行真实签名。需要发行者取得可用的正规签名证书或签名服务，然后在签名后的最终文件上重新扫描、计算发布哈希和测试。签名不是当前告警已经消除的证据，也不能承诺零误报。

如果 Pulse 以符合要求的开源项目发布，可研究 SignPath Foundation 免费签名服务：它要求 OSI 开源许可、已发布且维护的项目、可验证的仓库到产物构建、签名政策及一定项目信誉等。其证书发行者是 Foundation，并非向任何个人直接发证。尚未核验 Pulse 的全部依赖、发布状态和项目条件，不能宣称已符合或必获批准。

来源：[Kaspersky 数字签名建议](https://support.kaspersky.com/kes-for-windows/14.2/settings/protection/15957)、[SignPath Foundation 条件](https://signpath.org/terms)。

## 4. 工程方向：独立的管理员 worker

Microsoft 推荐主程序以 asInvoker 运行，必要的管理员操作由独立 helper 接受标准 runas 授权。Pulse 当前通过重新运行主 EXE 的 --file-operation-worker 实现进程隔离，但管理员进程仍是包含整个 UI 和应用入口的主程序镜像。

可进一步把文件操作 worker 放入既有 Pulse.Shell.exe：transport 已接受同目录 Pulse.Shell.exe 的身份；需要迁移 RunWorker 入口、父进程 helper 选择、OpsManager worker 依赖，保留同用户/进程身份/管道 ACL 校验，保证缺失 helper、UAC 拒绝、取消、断连和部分完成状态不回放。主程序不再承担管理员 worker 入口，具有独立的最小权限设计收益。

这是可解释的架构改进，但模块对照没有证明当前 worker 是规则触发原因。未实施、未扫描改造后的真实产物、未验证 UAC 和文件操作回归，不能把它当作确定会清除告警的方案；不应只因当前分类名称而大规模改造。

来源：[Microsoft 最小权限与 helper 建议](https://learn.microsoft.com/en-us/windows/win32/secbp/running-with-administrator-privileges)。

## 本轮没有证实有效的办法

- 更新病毒库：成功，当前样本仍被检出。
- 明确 asInvoker/uiAccess=false：构建和嵌入清单校验通过，仍被检出。
- 同源码独立干净构建：完成，代码节与现有构建一致，仍被检出。
- 两个模块程序及 Shell helper 未检出：有助于缩小问题，但没有定位到单一 API 或代码缺陷。

建议先针对当前样本复核，长期接入正规签名；如项目满足条件，再申请厂商 Allowlist 或开源签名服务。独立管理员 worker 应按权限隔离收益单独评估，实施后才能说明实际 AV 结果。测试目录、安装目录和便携包本轮没有被新诊断产物覆盖。
