# 1.1.3.4 构建与清理记录

日期：2026-10-21。代码提交：`53e32af`。本记录区分本轮实测、同一工作区此前的验证，以及已完成的安装替换。

## 改动与本轮验证

- 中、大、超大图标的选择蓝线由32增至48 DIP，保留窄卡片限宽。
- 新增 `pulse_file_selection_ui_test --selection-only`，直接检查生产绘制结果中的连续蓝色像素宽度，旧32 DIP不能满足44 DIP阈值。本轮84 PASS / 0 FAIL，覆盖三种图标大小、深浅主题、100%/150% DPI、悬停及非活动多选。已查看[实际渲染图](release-cleanup-2026-10-21/selection-48dip.png)；[日志](release-cleanup-2026-10-21/selection.log)。
- 使用MSVC/NMake Release、`PULSE_WITH_SELFTEST=OFF`，重新配置后构建五个生产目标；最初增量构建未刷新版本资源，发布检查拦截后已重新配置并纠正。五EXE的FileVersion、ProductVersion均为1.1.3.4，OriginalFilename逐项匹配。
- 便携包40个文件逐项与stage比对SHA-256，未包含测试/对象/日志等开发产物；主程序无内嵌selftest分发器。检查五EXE、LumaText、PDFium及7zip的依赖，未发现需要额外打包的VCRUNTIME/MSVCP/CONCRT。详见[载荷清单](release-cleanup-2026-10-21/payload.json)和[依赖记录](release-cleanup-2026-10-21/runtime-dependencies.txt)。
- 用户指定的 `E:\Projects\TestPrograms\Pulse` 已同步，40个文件哈希核对通过，替换前文件备份于 `E:\Projects\TestPrograms\Pulse-backup-20261021-203233`；[同步清单](release-cleanup-2026-10-21/test-program-deployment.json)。这项文件核对不等于可见界面验收。

本次提交同时收齐此前未提交的文本比较、标签和菜单动画、搜索历史防闪、预览目录分发、归档导航、窗口尺寸锁及Explorer交接修改。此前的定向测试和实际桌面证据分别见[文本比较](text-compare-tabs-2026-10-21.md)、[动画跟进](animation-followup-2026-10-21.md)、[标签关闭](tab-close-2026-10-21.md)、[预览修复](preview-audit-2026-10-21.md)、[归档](archive-sidebar-2026-10-21.md)、[尺寸锁](window-size-lock-2026-10-21.md)、[Explorer审查](../explorer-takeover-review-2026-10-19.md)。本轮未重复运行这些检查或完整历史套件。

## 清理与恢复

- 删除475份旧文档/附件，5,575,657字节；摘要见[历史维护摘要](../history-summary.md)，逐项路径/原SHA见[文档清单](../history-cleanup-manifest-2026-10-21.json)。475份删除项均与清理前快照哈希匹配，文档清理阶段80份近期受保护证据未变，50个保留的本地链接/锚点检查通过。提交时仅规范化日志换行及行尾空白；原始日志仍在清理前快照或诊断ZIP中。
- 删除8297个生成测试文件和测试构建产物，4,791,777,613字节；删除62个旧本地诊断文件，4,746,885字节；删除171个旧stage/重复备份/发布诊断文件，76,191,031字节。共9005文件、4,878,291,186字节（约4.54 GiB逻辑大小，不是磁盘空闲空间实测）。[生成产物](release-cleanup-2026-10-21/artifact-cleanup.json)、[旧诊断](release-cleanup-2026-10-21/diagnostic-cleanup.json)、[旧包](release-cleanup-2026-10-21/package-cleanup.json)均有逐路径清单。
- 安装替换验证完成后，另删除43个旧包/旧回退文件、47,045,697字节；最终共9048文件、4,925,336,883字节（约4.59 GiB逻辑大小）。见[最终清理清单](release-cleanup-2026-10-21/final-cleanup.json)。保留有效测试源码、当前五生产程序及其增量依赖、当前包和stage、依赖/许可证，以及最新完整回退目录。`build/bench_data` 中历史撤销身份记录未作为普通生成数据清除，原因见历史摘要中的F03保留边界。
- `dist/source-snapshot-before-cleanup-20261021.zip` 保存清理前源码/文档、未提交diff和快照清单。`dist/local-diagnostics-before-cleanup-20261021.zip` 归档1160份本地脚本和诊断记录，逐项读取校验通过。两份加密的生成测试JSON无法读取，不在诊断备份内；不是源码或用户配置。
- 旧报告中使用代码格式注明的 `bench_data/...` 路径是历史来源，清理后应在诊断ZIP中按原路径查找；未保留所有生成截图和大体积输入文件。近期交互报告已整理到docs的图证继续保留。

## 安装替换状态

目标为 `D:\Applications\Pulse`。已准备完整40文件备份/哈希验证、仅替换变化文件、异常回退及恢复PulseIndex服务的脚本。替换前未发现驻留主程序，服务为Running/Automatic，网络索引代理正在运行。

用户确认Windows UAC后，2026-10-21 21:51完成替换：仅五EXE和README.txt共6文件变化，安装目录全部40文件与1.1.3.4载荷哈希一致；完整旧载荷40文件备份于 `dist/rollback-before-1.1.3.4-20261021-215120`，部署后再次逐项核对通过。服务恢复为Running/Automatic，服务路径未改。网络索引代理PID33252，命令为 `"D:\Applications\Pulse\Pulse.Index.exe" --network-agent`，最终检查仍在运行。[部署结果](release-cleanup-2026-10-21/deployment.json)、[最终核对](release-cleanup-2026-10-21/installed-final.json)。

实际从安装目录启动隔离窗口，先查看[真实桌面启动截图](release-cleanup-2026-10-21/installed-start.png)确认窗口可见，再尝试点击图标视图。`--view`仅在截图模式应用，因此该可见窗口初始显示详情列表；这张图证明启动成功，不作为图标蓝线验收。第一次独立启动的窗口随后已不在进程列表，未确认退出原因；后两次保持测试会话时，前台PID检查未通过，均取消点击，没有把未执行的操作写成测试通过。

用户随后表示“我看着可以了”，并要求停止继续测试。按用户目测验收收尾，没有再进行窗口操作；蓝线的程序化验证仍以本轮84项生产渲染检查为准。本轮未运行完整历史回归，未重新构建安装器。
