# 架构整理与文件操作验证（2026-10-22）

## 范围

依据 `docs/architecture-review-independent-2026-10-22.md` 中的独立候选项，本次完成三个局部边界：

- 索引共享查询类型移至 `src/index/index_types.h`，保留字段、默认值及协议编码。
- `PrepareViewState` 显式推进标题、偏好、列状态、详情请求与重复文件页卷发现；`PrepareFolderSizes` 推进文件夹大小兴趣。命中检测和显示模型读取不再提交这些业务请求。
- `OpsManager` 直接持有菜单／文件操作回调所有者；打开请求由独立 mailbox 所有者承载。保留原打开线程有限等待及脱离策略。

保留工作区原有未提交修改。本次没有实施报告中全部候选重构。架构实施阶段未安装或部署；用户随后要求的安装目录替换已完成，过程与证据见 `deployment/README.md`，手工验收清单见 `deployment/manual-tests.md`。

## 实际 Z 盘问题及修复

真实映射盘测试暴露出两个功能问题：

1. 非空目录复制完成后，网络提供程序更新目录身份；仅保存创建时身份会使复制结果没有可用撤销。现在在创建租约保护下保留对象引用，提交撤销时复制已授权对象的句柄并读取当前身份，不重新按路径捕获替代对象。
2. 替换复制已提交，但旧撤销及 Cancel／Skip 冻结计划仍持有被替换对象的引用，导致备份删除保持 pending。现在只释放经实时身份确认属于被替换对象的旧引用；对应旧计划失去授权，不能重新接纳同名文件。仍执行原对象保护和删除后检查。

附带修正测试夹具生命周期：停止工作线程后释放测试会话撤销和恢复计划引用，再删除明确列出的测试文件。不通过递归删除未知剩余内容掩盖失败。

此前测试中对中文错误片段的比较，已改为比较现有本地化资源且要求非空；生产翻译未因这些断言改变。临时生产 API 诊断后缀已移除。

## 执行与证据

`scripts/validate_architecture_ops.ps1` 将选中测试和 Shell 主机按 SHA256 校验后复制到独立工作目录，在普通完整性会话中运行。每轮保留 stdout、stderr、退出码、超时、PASS／FAIL／SKIP 及载荷哈希。Z 盘测试要求实际卷类型为 `DRIVE_REMOTE`，只操作进程号和时间戳命名的本次夹具。

初次失败及中间诊断全部保留：根目录 `results.json`、`diagnostic-2`、`network-fix`、`network-fix-2`。`architecture` 中第一次视图／共享目录失败来自运行目录缺少 `bench_data` 父目录，补齐运行环境后 `architecture-recheck` 原程序哈希不变并通过。

已完成的独立检查：

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 搜索合并 | 23 PASS，24,192 种查询／分页组合零失败 | `architecture/search-merge.stdout.log` |
| 搜索客户端路由 | 21 PASS | `architecture/search-routing.stdout.log` |
| 真实 Shell IDataObject 虚拟文件粘贴，本地 | 5 PASS | `architecture/virtual-clipboard-local.stdout.log` |
| 同上，真实 Z 盘 | 5 PASS | `architecture/virtual-clipboard-network.stdout.log` |
| 显式视图准备及命中读取边界 | 23 PASS | `application-current/view-preparation.stdout.log` |
| 共享目录枚举／列请求取消 | 10 PASS | `application-current/shared-requests.stdout.log` |
| 实际详情元数据工作线程及共享大小结果 | 7 PASS | `application-current/details-worker.stdout.log` |

最终文件操作回归记录在 `final-ops/results.json`，共 **544 PASS、0 FAIL、1 SKIP**。实际 Z 盘三组共 **70 PASS**：

| 检查 | PASS | 覆盖重点 |
| --- | ---: | --- |
| shell-completion | 14 | 菜单／打开／文件操作完成结果归属 |
| open-workers | 3 | 五窗口延迟打开线程的有限关闭与晚到完成 |
| mutation-undo | 180 | 创建、重命名、移动、部分完成及撤销 |
| undo-identity | 61 | 同名替换与撤销对象身份保护 |
| source-contract | 125 | 源对象稳定性、目录／链接保护、冻结计划失效及大文件字节 |
| local-rename | 53 | 普通及批量重命名、冲突、取消、撤销 |
| delete-confirmation | 5 | 删除确认与同名替换 |
| delete-locked | 5 | 锁定文件错误及失败状态 |
| network-transfer | 23 | 真实 Z 盘双向复制、冲突取消／跳过／替换、空文件、中文空格路径、跨卷移动／撤销、清理 |
| network-undo | 37 | Z 盘复制／移动／重命名撤销、非空目录复制撤销、连续重命名、同名替换拒绝 |
| network-delete | 10 | Z 盘永久删除确认、取消保留、替换拒绝、重新确认及实际文件／目录删除 |
| transfer-control | 11 | 暂停／继续、取消、字节进度及替换回滚 |
| directory-skip | 17 | 文件／目录冲突跳过、嵌套兄弟项及撤销证据 |
| clipboard-isolated | 0 | 1 SKIP，退出 77，未执行断言 |

最终定向构建成功：`pulse_ops_test`、`pulse_rename_ops_test`、`pulse_recycle_backend_test`、`pulse`、`pulse_directory_contract_application_test`。主程序构建包含受影响的 `pulse_index`（含网络代理入口）、`pulse_shell` 依赖；此前另有索引及搜索目标构建验证。最终载荷 SHA256 见 `final-build.json`；测试暂存载荷哈希见各轮 `results.json`。最终应用程序夹具重建后再次运行，结果见 `application-current`。

可复现命令（在 x64 VS 环境中构建，普通完整性 PowerShell 中执行测试；输出目录应每轮唯一）：

```powershell
cmake --build build --target pulse_ops_test pulse_rename_ops_test pulse_recycle_backend_test pulse pulse_directory_contract_application_test
pwsh -NoProfile -File scripts/validate_architecture_ops.ps1 -CaseName shell-completion,open-workers,clipboard-isolated,mutation-undo,undo-identity,source-contract,local-rename,delete-confirmation,delete-locked,network-transfer,network-undo,network-delete,transfer-control,directory-skip -OutputDir docs/validation/architecture-ops-rerun
pwsh -NoProfile -File scripts/validate_architecture_ops.ps1 -CaseName view-preparation,shared-requests,details-worker -OutputDir docs/validation/architecture-app-rerun
```

包含不可用的剪贴板组时，脚本以 77 表示未完整执行，不能将其当作所有检查通过。最终各网络夹具自行清理成功；早期失败留下的四个目录在确认空目录后仅做非递归删除，记录见 `fixture-cleanup.json`。

## 验证边界

- 私有剪贴板站创建在当前会话返回 Win32 183；测试明确 SKIP／退出 77，执行零条剪贴板断言，未清空或覆盖用户剪贴板。虚拟文件 IDataObject 粘贴通过不能代替 CF_HDROP 系统剪贴板往返验证。
- 文件操作通过生产执行路径、真实文件字节及对象身份检查验证；没有把这些结果表述为键盘 Ctrl+C／Ctrl+V 或安装版本可见 UI 的完整人工验收。
- 未做网络中断、其他网络提供程序、所有主题／DPI 或真实第三方 Shell 扩展的完整验证。
- 现有 UI 依赖检查仍报告 `src/ui/text_diff_window.cpp/.h` 到 `src/app/text_diff.h` 的两项边界违规；这两个文件未在本次任务中修改，未扩大修复。
