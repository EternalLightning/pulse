# 右键菜单与文件夹操作验证（2026-10-29）

本次范围：菜单项在分割线之间的竖直居中、最近变化入口服从功能开关，以及上游“合并到文件夹 / 解散文件夹”。保留共享工作区已有修改。

上游来源为 [be50f0f4 的 app_commands.cpp](https://github.com/jimmgreen/pulse/blob/be50f0f4c6bcedb9cfdb2d0cb97b1b0471531b26/src/app/app_commands.cpp#L252-L419)，按本地后台操作队列和身份保护机制适配，没有合并整条上游历史。

## 实现

- 菜单分割线绘制在额外留白区域中央，布局和绘制共享分隔区域高度；命中仍排除分隔区域。
- 文件夹菜单、空白处菜单都显式传入 `change_tracking_enabled`，关闭设置时不加入最近变化项。
- 至少两个同目录实际项目可合并；一个或多个普通文件夹可解散。拒绝虚拟位置、只读位置、驱动器根和解散目录链接。
- 名称输入使用现有对话框；目录枚举、创建、移动、删空和撤销均在操作线程执行。同名内容保留，支持 `X/X` 解套。
- 一次撤销按组恢复空容器、移回内容，再恢复外层名称。容器仅通过原身份句柄删除空目录；新到内容或路径替换阻止清理。
- 组合撤销和目录基本时间、属性可持久化。`X/X` 重建后只对同组、同路径且原身份匹配的外层重命名记录更新新目录身份。

## 实际检查

Release、Ninja、MSVC x64，生产配置 `PULSE_WITH_SELFTEST=OFF`。构建目标为主程序、必需辅助程序及下表定向测试，没有运行完整 selftest 或完整文件操作套件。

最终主程序、生产 Shell 和定向测试构建均已成功退出；见[初次目标构建](build-final.log)、[租约修复构建](build-folder-repair.log)、[最终撤销修复构建](build-undo-repair.log)。[交付文件身份](build-identity.json)记录最终现有构建产物及 SHA256。

| 命令 | PASS | FAIL | SKIP | 证据 |
| --- | ---: | ---: | ---: | --- |
| `pulse_folder_actions_menu_test.exe` | 32 | 0 | 0 | [菜单条件与中英文资源](menu-actions.log) |
| `pulse_menu_text_test.exe --separator-centering` | 25 | 0 | 0 | [四个 DPI、明暗主题的布局、命中与绘制](separator-centering.log) |
| `pulse_folder_actions_test.exe` | 21 | 0 | 1 | [合并、解散、冲突、组合撤销、取消、替换和清理](folder-actions.log) |
| `pulse_ops_test.exe --filter mutation-undo` | 180 | 0 | 0 | [普通移动、重命名、部分撤销和身份保护](mutation-undo.log) |
| `pulse_ops_test.exe --json-parser-only` | 8 | 0 | 0 | [旧历史兼容和损坏数据拒绝](json-parser.log) |

合计 **266 项定向断言通过、0 失败、1 项跳过**。符号链接创建返回 1314，未把该夹具记为通过；菜单模型的 reparse 选择限制另有覆盖。上述历史解析断言不等同于完整文件操作回归。

已查看 `separator-{light,dark}-{100,125,150,200}.png` 全部八张生产绘制预览，检查分割线、中文完整性、快捷键和悬停区域，无此次改动造成的裁切或重叠。

## 运行环境与修复经过

本机从构建目录启动的原生测试进程无法创建夹具（错误 5）。按仓库已有 `scripts/test_file_operations.ps1` 的做法，将测试与生产 `Pulse.Shell.exe` 复制到独立临时 runtime，逐个校验 SHA256 后运行；没有修改 ACL、产品权限或弱化测试。[最终副本与哈希](runtime.json)。截图使用已有 stdout PNG 流由父 harness 保存。

首轮功能测试确实发现了目录祖先租约冲突，随后发现 `X/X` 撤销的重建身份链问题；两处均已修复后重跑受影响检查。[首轮失败](folder-actions-first.log)、[租约修复后剩余边界](folder-actions-lease-repair.log) 保留，未计入最终通过数。日志中的预期失败场景诊断不是断言失败。

共享目录并行构建曾导致 Ninja restat 文件占用，等待其他构建退出后重试。现有综合 controller 夹具存在会话布局链接冲突，本次新增独立菜单入口运行相同菜单用例，保留原测试。

## 验证边界

- 解散后撤销重建容器不恢复自定义 ACL；移动内容会按系统行为更新目录修改时间。
- 未进行真实 UNC 共享、实际超长目录树或完整交互窗口流程验收。UNC 菜单资格与扩展盘符根路径验证有定向覆盖。
- 本次没有打包、发布或替换已安装程序。
