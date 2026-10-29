# 文件操作补充验证：2026-10-25

## 用户桌面会话复测（最新结论）

用户在 `E:\Projects\TestPrograms\Pulse` 运行测试包后，读取并归档 `results\run-27464-20158` 的实际输出。三个二进制的 SHA256 均与测试包清单一致。本节更新验收结论，下文保留早先受执行环境限制的诊断记录。

| 范围 | PASS | FAIL | 退出码 |
|---|---:|---:|---:|
| 本地 E: 盘内 | 46 | 0 | 矩阵整体 0 |
| Z: 盘内 | 44 | 0 | 矩阵整体 0 |
| E:→Z: | 21 | 0 | 矩阵整体 0 |
| Z:→E: | 21 | 0 | 矩阵整体 0 |
| 永久删除后端，本地 | 26 | 0 | 0 |
| 永久删除后端，Z: | 23 | 1 | 1 |

文件操作矩阵合计 **132 PASS / 0 FAIL / 0 BLOCKED**，包含生产 Shell 创建、确认永久删除、本地回收及回收撤销；本轮本地和网络夹具清理均通过。此前 Shell 连接失败涉及的功能在用户桌面会话中已验证通过；这不能反推先前受限环境中的连接错误根因已经查明或修复。

Z: 后端唯一 FAIL 是只读回滚夹具前提：属性设置返回成功，但删除前读回 `128 / NORMAL`，因此输出 BLOCKED，未执行故障注入及回滚断言。当前结果不足以证明产品回滚缺陷，也不能宣称该分支通过。本地对应故障注入及属性恢复通过。目录符号链接用例本地以 Win32=1314、网络以 Win32=1 跳过，均未计为通过。

证据：[矩阵](user-run-27464-20158/matrix.log)、[本地后端](user-run-27464-20158/backend-local.log)、[网络后端](user-run-27464-20158/backend-network.log)、[退出码](user-run-27464-20158/exit-codes.txt)、[二进制哈希](user-run-27464-20158/binary-hashes.json)。本轮只读取用户运行结果并归档，没有修改产品代码或再次执行文件操作。

与原审计的关系：P-01 涉及的本轮永久删除完整调用链和后端基础场景、P-06 涉及的本地回收撤销场景已有通过证据；不据此扩大为原条目全部边界或结构性改动均已完成。尚未覆盖 Z: 只读回滚、符号链接、网络断连/重连、长时间大文件压力及真实可见 UI 交互。早先两处网络残留目录没有在本轮检查或清理。

本轮按用户要求补充本地 E:、网络 Z:、E:→Z:、Z:→E: 的真实文件操作测试。Z: 当前映射为 `\\server\Tbox`。复用现有 `pulse_ops_test`，新增 `--file-operation-matrix LOCAL_PARENT [NETWORK_PARENT]` 筛选入口；没有新增测试可执行目标，也没有修改产品权限、删除策略或安装文件。

核心用例通过生产 `OpsManager/OpRequest` 执行，创建和删除仍走生产 Shell 宿主。Win32 直接创建仅用于独立夹具准备，不能计为产品创建功能通过。每次在指定父目录下创建随机、排他的子目录；检查操作状态、精确字节内容、源路径去留、目录结构和临时文件残留。

新增覆盖：文件/文件夹创建；连续文件重命名及两次撤销；非空目录重命名；文件和非空目录的复制、移动、移动撤销；中文与空格文件名；零字节文件、空子目录、跨缓冲区长度的二进制内容；冲突取消、跳过、替换、保留两份；取消永久删除、确认文件删除、父子重叠选择的递归删除；本地默认回收及撤销、网络默认删除的永久确认。

独立准备各类夹具，创建操作失败不会阻止重命名或传输用例继续执行。所有撤销后的内容断言均要求撤销真正完成，不能因原文件还在就记为“恢复成功”。

## 构建与完整调用链结果

仅构建 `pulse_ops_test`（含所需的 `pulse_shell` 依赖）和已有 `pulse_permanent_fileop_console_test`；VS2022 x64 Release，独立 `build_audit_msbuild_20261025`。最终相关构建成功，未见新警告。见 [矩阵构建](file-operation-matrix-build-final.log)、[后端最终构建](file-operation-backend-final-build.log)。初次编译诊断也保留在 `file-operation-matrix-build-initial.log`。

从 build 目录直接运行仍在进入 main 前返回 `0xC0000142`，没有执行断言，见 [直接运行记录](file-operation-matrix-runtime.json)。普通临时目录中的 SHA256 一致副本可以进入 main；运行器同时复制同一次构建的 `pulse_ops_test.exe` 和 `Pulse.Shell.exe`，未修改执行令牌或生产 DACL。

最终矩阵实际结果如下。数字为断言数量，多个断言可能对应同一项操作；不能将通过断言数当作独立场景数，也不能把整个矩阵描述为通过。

| 范围 | PASS | FAIL | 结论 |
|---|---:|---:|---|
| E: 盘内 | 34 | 12 | 重命名、目录重命名、连续撤销、复制/移动/撤销及四种冲突处理通过；创建和删除完整链未通过 |
| Z: 盘内 | 33 | 11 | 同类重命名和传输用例通过；创建和删除完整链未通过；本轮清理还有一次失败 |
| E:→Z: | 21 | 0 | 文件/目录复制、移动、移动撤销及四种冲突处理通过 |
| Z:→E: | 21 | 0 | 同上，反向通过 |

最终合计 **109 PASS / 23 FAIL，exit 1**。证据：[完整输出](file-operation-matrix-20261025-162521-312.log)、[路径、哈希和计数](file-operation-matrix-20261025-162521-312.json)。较早两轮日志保留用于诊断，不与最终计数合并。

22 条失败断言涉及生产 Shell 链：连接报 `pipe CreateFile err=5`，创建状态为 `0x800700E9 / shell host unreachable`；确认删除报告结果未知并停止破坏性重试。本地回收及其撤销因此也未验证通过。取消删除确认本身通过。失败日志全部保留，不能据此声称安装版同样失败，也不能声称完整删除或回收功能已通过。

另 1 条失败为 Z: 夹具清理返回 `145 / ERROR_DIR_NOT_EMPTY`，与 Shell 传输失败分开记录。本地最终夹具已清理。

## 永久删除后端的补充验证

已有 `pulse_permanent_fileop_console_test` 增加 `--root-parent PATH`，同一套断言可以使用本地或网络父目录；再增加 `--filter readonly-rollback`，便于只复查相关属性边界，不重跑全部操作测试。保留原来的默认入口，夹具根改为排他创建。

直接调用生产 `DeletePermanentAuthorized` 的验证绕开的是测试范围中的 IPC 环节，**不替代 OpsManager→Shell 的完整调用链验收**。

| 首轮后端运行 | 结果 | 证据 |
|---|---|---|
| 本地 | 26 PASS / 0 FAIL | [日志](file-operation-backend-temp-local.log) |
| Z: | 25 PASS / 1 FAIL | [日志](file-operation-backend-temp-network.log) |

两端均实际通过普通文件、非空目录删除、批次身份预检、同名替换保护、取消及部分完成报告。本地还验证了只读子项；Z: 对属性设置返回成功不代表只读属性实际生效，因此不认定其只读子项语义已验证。符号链接用例分别因 Win32=1314、Win32=1 明确 SKIP，没有计为通过。以上夹具均已清理。[二进制哈希](file-operation-backend-temp-artifact.json)和各运行的同名 JSON 保留。

Z: 的失败位于只读回滚夹具。定向诊断发现：`SetFileAttributesW(READONLY)` 返回成功，但**删除之前**读取的属性就是 `128 / NORMAL`。生产删除没有进入清除只读再恢复的分支。该证据不足以证明产品的只读回滚有缺陷；当前 Z: 无法建立这个用例所需的只读前提。

已加强夹具前置断言：必须实际观察到只读属性才执行故障注入；前提不满足仍保留 FAIL，并输出 BLOCKED，不将未执行的回滚断言伪装成通过。最终只运行该筛选：本地 **4 PASS / 0 FAIL**；Z: **1 PASS / 1 FAIL**（失败为夹具前提，回滚分支未执行）。见 [本地](file-operation-readonly-final-local.log)、[Z:](file-operation-readonly-final-network.log)、[最终诊断哈希](file-operation-readonly-final-artifacts.json)。未放宽生产身份检查或增加按路径重试。

## 残留与限制

首轮 Z: 夹具也出现清理失败。后续清理命令被自动审批拒绝，返回的理由只有 `blocked by policy`。未换用其他命令或接口删除该目录。最终一轮另有一处清理失败。仍保留的自建目录是：

```text
Z:\pulse-fileops-f8820ee6734e0e31a15c328247c6c34a
Z:\pulse-fileops-ef8fa8cb7594bd205febe15020bb71a0
```

中间一轮两个夹具均清理成功；永久删除后端及最终只读筛选的夹具也均清理成功。普通临时目录中的测试二进制副本保留供复核，其准确路径见相应 JSON，不涉及用户偏好或用户原文件。

尚未完成：生产 Shell 连接可用条件下的创建、确认永久删除、回收与回收撤销；Z: 的只读回滚分支；网络断连/重连、长时间大文件压力、真实可见 UI 交互。这轮没有运行全部 ops 测试、完整 selftest 或安装/发布步骤。

与原审计的关系：P-01 的永久删除后端得到本地及真实 Z: 验证，完整调用链仍受上述连接失败影响；涉及回收撤销的 P-06 不能由这些后端结果替代。第8、9部分尚未完成的结构性改动没有因为新增测试而改记为已完成。

## 重跑

构建完成后，在具备正常桌面和网络权限的会话运行；指定的两个父目录必须已存在，测试只创建并操作其随机子目录：

```powershell
& 'E:\Projects\pulse\scripts\test_file_operations.ps1' `
    -BinaryDirectory 'E:\Projects\pulse\build_audit_msbuild_20261025\Release' `
    -LocalParent 'E:\Projects\pulse\build_audit_msbuild_20261025\console' `
    -NetworkParent 'Z:\' `
    -EvidenceDirectory 'E:\Projects\pulse\docs\validation\audit-followup-20261025'
```

运行器保存每次独立的日志、计数、夹具路径、退出码及二进制 SHA256，保持失败退出码。原生后端入口为 `pulse_permanent_fileop_console_test --root-parent PATH`；只读定向入口再追加 `--filter readonly-rollback`。
