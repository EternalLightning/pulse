# Pulse 1.1.1.0 预览程序告警与修正记录

记录日期：2026-10-11。当前结果：修正共享内存 ACK 条件后，本机 Kaspersky 对新构建及原安装位置均未检出；两组预览检查 43 PASS / 0 FAIL，安装目录和新 ZIP 的 40 文件核验一致。没有提交厂商复核，不能宣称已得到官方误报结论或保证其他机器、未来云规则的结果。

## 版本差异和原因边界

| 对象 | 长度 | MD5 | 本机对照扫描 |
|---|---:|---|---|
| 保留的 1.1.0.1 安装备份 | 264,192 | C15CDC84BD1B6A3218C6EF7A6A090702 | 正常 |
| 首次 1.1.1.0 载荷 | 264,704 | BD23B0E1800B2E481738BEF849E90248 | VHO:Trojan-PSW.Win32.Convagent.gen |
| ACK 资源状态修正后的 1.1.1.0 | 264,704 | A4D871DA4C5CFB1C916388607E786048 | 0 detected |

保留的备份 EXE 时间为 2026-10-10 17:00:20，版本来自 PE 文件信息。相对当前 HEAD，预览主文件只有一处 ACK 条件改动；同目录的其他预览实现、崩溃日志实现未改动。构建配置增加了诊断导出的本地化静态依赖、版本从 1.1.0.1 改为 1.1.1.0；Ninja 的预览链接命令多出 pulse_localization，但前两版实际 DLL/API 导入一致，未发现新增直接网络或凭据 API。

首次新版比旧版代码节虚拟长度增加 128 字节，文件因节对齐增加 512 字节，资源节仅 8 字节改变；详细数据见 preview-section-comparison.json。这些差异与代码/版本变化一致，但没有厂商检测规则，不能据此证明某个机器码或 API 是触发原因。

## 实际修正

旧版以 mapping 句柄非空决定等待 ACK：句柄创建成功而 MapViewOfFile 失败时，producer 不会发送映射名称，却仍可能等待确认。前次修订将条件换成映射名称非空，解决等待错误，但把资源准入绑定到了附带字符串。

本次在 src/preview_host/main.cpp 只将该条件改为 ok && view。有效像素视图是已创建且可访问的共享像素缓冲区；响应、名称和正文写入成功后才等待消费者确认。映射创建失败、视图创建失败、文本/属性/失败响应均不等待像素确认。全部原有失败状态和 UnmapViewOfFile/CloseHandle 清理保留。没有撤销失败保护，没有改名、混淆、改变权限要求或删除崩溃诊断。

## 实际验证

- 只构建 pulse_preview、pulse_preview_test，构建通过，无新增编译警告。
- --response-compatibility-only：11 PASS / 0 FAIL，真实 helper 的图像、文本、Hex、属性及错误响应。
- --thumbnail-transport-only：32 PASS / 0 FAIL，含成功 ACK、映射失败响应、传输超时、取消和不合法 schema。映射失败用例是传输夹具，没有强制真实 producer 的 MapViewOfFile 失败；不能把它描述为真实资源不足注入测试。
- 首次直接运行夹具未能创建临时目录，尚未进入预览逻辑。保留初次失败日志后，用逐项哈希一致的普通权限临时副本运行，两组均退出 0。
- Kaspersky 21.26 对首次载荷和备份的同一轮扫描：处理 2 对象，1 OK、1 detected。对修正后 build 文件扫描：1 OK、0 detected、退出 0；复制到原安装位置后扫描同样 1 OK、0 detected、退出 0。病毒库时间显示 2026-10-11 08:59:00。
- 本机帮助确认 /i0 为 Report only；CLI 控制台仍提示非交互模式忽略动作参数，实际报告 Settings 为 Report only、Try disinfect/delete = No。本次记录中没有清除、删除、关闭防护或排除操作。
- 新组件部署到 D:\Applications\Pulse 后，40 个运行文件的长度/SHA-256 全部一致。其他文件及配置保留。未额外做完整 UI/第三方 provider 验收。

## 当前交付

便携包：dist/Pulse-1.1.1.0-portable-win-x64.zip，11,714,038 字节。SHA-256：062770533A853ED6D3E47EBC97770E05C037DF5CCB48CB4C6141E8F81F20F503。新预览组件 SHA-256：E41AF62742478CC9752F3D1A1BC68BF3AEE8383A528094C6225C82A3ECA40B4F。

首次被告警 ZIP 的 SHA-256 为 B2B3C9566A5829323490676D550A7D6A513C38C92607364D997599C6F448FCD0，原文件已由新包替换；旧哈希只用于历史追踪，不能校验当前包。antivirus-detection.json 保留最初核查时的哈希与路径快照，preview-view-state-verification.json 记录当前状态。

## 核查范围和未确认事项

MSVC 编译器/链接器/资源编译器的 Authenticode 状态为 Valid、签名者 Microsoft Corporation；Ninja 和预览 EXE 未签名。未签名不是恶意证明，也不能直接解释本次检测。PE 保留 ASLR、DEP、Control Flow Guard，这些设置不构成安全认证。

有限范围静态源码和导入扫描未发现直接网络请求、凭据提取或跨进程注入。OpenProcessToken/GetTokenInformation 用于当前用户 SID 和 IPC 权限；SVG 动态 DLL 从 System32 加载。COM/Shell 提供程序和图像解码器可在运行时载入其他组件，静态直接导入表无法覆盖它们。没有完整安全审计、独立重建证明或厂商规则分析。

按用户要求停止复核申请；没有上传样本或源码。未依据告警名称添加功能特判。当前结果只表明这次资源状态修正后的实际构建在本机扫描及复制时未检出。若同一哈希再次告警，应保留事件进一步定位，不能自动放行。

## 证据

- antivirus-detection.json、deployment.json、preview-view-state-verification.json。
- preview-build-commands.txt、preview-build-dependencies.txt：首次载荷构建输入记录。
- preview-pe-imports.txt、preview-pe-headers.txt：首次被告警文件的静态记录；preview-previous-* 对应安装备份。
- kaspersky-preview-comparison.log、kaspersky-preview-view-state.log、kaspersky-installed-preview-view-state.log。
- preview-view-state-tests.json、两组测试日志、preview-response-view-state-initial-environment-failure.log。
