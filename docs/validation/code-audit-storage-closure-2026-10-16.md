# 存储迁移身份与诊断枚举闭环（2026-10-16）

## 范围

对应 outstanding 的 R04、R05。只修改 `src/common/user_storage.{h,cpp}`、`src/common/diagnostics_exporter.{h,cpp}` 和配置迁移/诊断专用测试。保留已有 dirty 修改；不修改索引迁移、CMake 或真实用户配置/日志。动态夹具置于仓库 `bench_data/configuration-identity-*` 与 `bench_data/diagnostic-enumeration-*`，保留供检查。

## R04：已复现路径回滚误删替换对象

主代理串行构建并运行修复前的 `pulse_user_storage_test --configuration-identity`，退出 1。复制校验返回后，夹具把目标 app.json 改名，并写入另一个写入者拥有的新 app.json，再强制迁移失败。结果 `replacement=1 directory_rename=0`；“rollback never deletes replacement object”和“destination file and directory retain leases until rollback”失败，排程、触发失败、保留 active/pending/error、保留 unrelated 文件通过。该对照确认旧路径 DeleteFile 回滚误删替换对象，而不是仅凭源码推测。

修复后目标以 CREATE_NEW 创建，保留具有 DELETE 权限、拒绝写入及删除共享的同一句柄，复制、Flush、逐字节校验均使用句柄；目标在提交或回滚前不能被改名替换。失败通过 FileDispositionInfo 删除本次持有对象，不按后来解析出的路径删除。源文件用 OPEN_REPARSE_POINT 打开并检查句柄属性，源/目标及已有祖先目录持有拒绝写入/删除共享的目录句柄，取得租约后重新验证目标。旧 ContentIndex 配置的父目录也取得租约。

受管 wallpaper 路径更新通过同一尚未发布的 app.json 句柄写入并 Flush，避免原 atomic replacement 使持有句柄与发布对象分离。全部复制对象在 locator 保存成功后才解除回滚责任；失败仍保留旧 active 和 pending/error。目标不包含本次持有对象的文件不参与回滚。

## R04 追加：迁移不能静默丢失文件元数据

复核发现用默认数据流手工复制替代 CopyFileW，会遗漏 ADS，并可能把 EFS 源文件发布为普通明文。这是行为回退，不能只列为“未验证限制”。新增 `pulse_user_storage_test --configuration-metadata`：在隔离 app.json 上建立实际 `:metadata` 流，直接 CopyFileW 对照必须保留该流，默认流复制对照证明流丢失；真正的迁移必须拒绝此源文件、保留旧 active/pending 与原流，并不遗留不完整目标文件。初版元数据 selector 为 15 PASS / 0 FAIL；主代理最后重编并运行包含 wallpaper ADS 的版本，17 PASS / 0 FAIL，退出 0，无 SKIP，EFS 实际创建成功并验证迁移拒绝。

采取明确拒绝无法保留内容的策略：在创建目标前，通过源句柄读取普通属性、资源安全属性和 BackupRead 备份流；只接受默认无名 BACKUP_DATA。发现 ADS、EA、其他备份流、资源属性 ACE 或无法完成元数据查询时拒绝整次迁移。EFS、压缩、稀疏、offline 等不在普通属性允许集合中的文件也拒绝，不把它们改写成普通文件并切换 root。读取备份数据采用 64KiB 缓冲解析流头，使用 BackupSeek 跳过默认流负载，完成或失败均释放备份上下文。

普通 hidden/system/archive/readonly/temporary/not-content-indexed 属性与创建、访问、最后写入时间通过同一目标句柄保留；受管 wallpaper 路径发生内容改写的 app.json 保留新写入/更改时间。元数据设置或 Flush 失败会回滚。只在拥有的目标句柄上清除 readonly 以允许 FileDispositionInfo 删除本次副本，不使用路径替代删除。

元数据 selector 还包含普通属性/时间实际比对、locator 保存失败后的 readonly 目标回滚、真实 sparse 源拒绝；追加 wallpaper ADS 用例检查普通 app.json 已复制后发现不支持元数据时回滚早先副本。EFS 初始化若不可用会输出 SKIP 和 Win32 错误，不算作验证通过；若可用则检查迁移拒绝且原文件仍加密。

## R05：已复现枚举失败被当作成功

主代理串行运行修复前的 `pulse_crash_test --enumeration-errors`，退出 1，3 PASS / 6 FAIL。FindNextFile 接缝注入 ERROR_READ_FAULT，旧逻辑仍报告导出/清理成功、写出成功 manifest，空目录检查也接受枚举失败；存在但非目录的 Crashes 路径被当成无日志。正常导出、正常清理与可选目录明确缺失的对照通过。

修复区分 FindNextFile 的 ERROR_NO_MORE_FILES 与其他终止错误，只有正常枚举完成才允许成功。清理首次/后续枚举失败填入既有本地化错误；目录存在但类型不符、重解析点或属性查询失败均不再视作可选内容缺失。根日志属性查询只有 FILE_NOT_FOUND / PATH_NOT_FOUND 可以跳过。导出失败保留已有部分副本并返回 false，不写成功 manifest；清理失败可能已移除部分报告，返回 false，不宣称原子清理。

最终故障夹具进一步在实际复制一个日志后注入错误，并断言部分副本存在而成功 manifest 不存在；该加强版另含完整枚举、缺失目录、非法目录对照。

## 修复后验证

主代理串行完成受影响目标构建和 selector 验证；存储代理另运行隔离、哈希一致的 Medium 副本排除默认测试的完整性级别限制，未使用共享 build：

| 检查 | 实际结果 |
| --- | --- |
| `pulse_user_storage_test --configuration-identity` | 6 PASS / 0 FAIL，退出 0 |
| `pulse_user_storage_test --configuration-metadata` 最终版 | 17 PASS / 0 FAIL，退出 0，无 SKIP；包括 app/wallpaper ADS、普通元数据、readonly 回滚和实际 sparse/EFS 拒绝 |
| `pulse_crash_test --enumeration-errors` | 10 PASS / 0 FAIL，退出 0；包括实际复制部分日志后枚举失败及无成功 manifest |
| `pulse_user_storage_test` 默认配置迁移专用测试 | 哈希一致的新建 Medium 副本，实际 child token RID 8192；30 PASS / 0 FAIL，退出 0。早先 Low 运行失败保留为环境对照，不计通过 |
| `git diff --check`（本组文件） | 无输出，通过 |

相关检查完成后停止验证，未运行完整应用/崩溃测试套件。最终日志：[身份](audit-closure-2026-10-16/pulse_user_storage_test-configuration-identity.log)、[元数据](audit-closure-2026-10-16/pulse_user_storage_test-configuration-metadata-final.log)、[诊断枚举](audit-closure-2026-10-16/pulse_crash_test-enumeration-errors.log)、[默认迁移 Medium](audit-closure-2026-10-16/pulse_user_storage_test-default-medium-token8192.log)。

## 默认迁移测试的完整性级别环境对照

早先直接运行默认测试无 stdout、异常退出 `0xC0000409`；命令局部设置仓库 TEMP/TMP 后，日志明确出现 registry locator 保存 `system error 5`。主代理观测 build exe 与仓库复制 exe 均为 Low Mandatory NW，尽管启动 PowerShell 为 Medium，且父进程能写入唯一隔离 StorageTests 注册表键。对仅该隔离目录/文件执行 `icacls /setintegritylevel M` 也被 Access denied 拒绝。上述失败过程和原日志保留，没有改整个仓库 ACL、原 exe 或产品逻辑来迎合环境。

存储代理通过 `File.ReadAllBytes` / `File.WriteAllBytes` 在真实 LOCALAPPDATA\Temp 新建文件，保持可执行文件内容一致而不复制 Low 标签。使用 CreateProcess 创建 suspended 子进程，从该实际 child 的 token 读取 TokenIntegrityLevel，再恢复其主线程；PID 8068，RID 8192（Medium），默认专用测试全部 30 项通过，退出 0。该差异对照支持先前 registry access denied 来自 Low 运行约束，而非本次配置迁移功能回归；早先无输出异常的具体抛出点未另行定位，不把其所有内部步骤推断为已解释。

源与 Medium 副本的 SHA256 均为 `77C8AE73D73BF79C481061ECFE5FF75691C5140D9A9B7D09119432A76DFC32D2`。Medium 副本和临时夹具位于 `C:\Users\EternalLightning\AppData\Local\Temp\PulseStorageMedium-d91dd4e291f04b4d903e44bf368f0b78`。后来追加 wallpaper 测试的最终 selector exe SHA256 为 `1DCE7DF3109BD7C8C855C38D4030B9926F745FCCCDE5F2C872311CFC07A9F87B`；默认测试结果绑定前一个确切哈希，不混写成后一个二进制的动态结果。

详细 [token/hash/失败过程证据](audit-closure-2026-10-16/pulse_user_storage_test-default-medium-token8192-evidence.txt) 和 [实际运行 helper](audit-closure-2026-10-16/pulse_user_storage_test-default-medium-probe.ps1) 已复制到仓库。helper 只创建独立 TEMP 副本、读取真实 child token、启动该专用测试；没有共享构建、生产设置写入或仓库 ACL 变更。

## 限制

夹具通过确定性同步接缝执行真实 Win32 文件替换及枚举错误，未覆盖长时间外部进程竞争、UNC/远程后端、磁盘损坏、断电、卷卸载、全部重解析类型。不支持的元数据采用拒绝迁移策略；不会宣称 ADS/EFS/稀疏已被完整复制。资源安全属性、EA 等拒绝分支目前仅有源码检查，尚无专门动态夹具。回滚删除若被文件系统拒绝，只可能遗留本次创建的副本；不会退回路径删除替代对象。诊断没有引入跨文件快照或原子清理保证，未触碰真实崩溃日志，也未运行真实崩溃/保留策略的整个测试程序。
