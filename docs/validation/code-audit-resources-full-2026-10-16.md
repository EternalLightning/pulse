# 公共资源文件全量静态审查（2026-10-16）

## 范围与方式

先读取仓库 AGENTS.md、工作区状态及本次 diff，保留现有未提交改动。完成静态审查后，按主代理明确授权仅修正下述一处中文说明，并新增本审查记录；不构建、不运行 GUI、不委派其他代理。

完整审阅以下 5 个文件，共 4519 行。localization.h 按完整代码段阅读；resource_ids.h、runtime_messages.inc 全文阅读；string_ids.h 的全部 1055 个数字 ID 与 strings.rcinc 的全部中英文本通过按定义顺序输出的 6 个有界批次配对阅读，同时单独读取头文件的非数字定义行与资源文件的全部结构行。批次区间为定义序号 [0,180)、[180,360)、[360,540)、[540,720)、[720,900)、[900,1055)。曾有一次合并原文输出被截断，后续已用有界段与完整配对输出补齐，未将被截断输出计为完整阅读。

| 文件 | 行数 | 审查内容 |
| --- | ---: | --- |
| src/common/localization.h | 1101 | Language、StringId 的全部映射、公开声明、Format 模板 |
| src/common/resource_ids.h | 49 | 47 个图标/二进制资源定义 |
| src/common/runtime_messages.inc | 77 | 76 个资源与运行时共用中英消息 |
| src/common/string_ids.h | 1098 | 1055 个 ID、编号唯一性、包含保护结构 |
| src/common/strings.rcinc | 2194 | 46 个 STRINGTABLE 的语言与边界、完整中英文本、运行时消息展开 |

为确定资源使用方式，定向读取 localization.cpp、app/pulse.rc，以及 ui_settings_view.cpp:234–260、index_engine.cpp:430–440，并检索移除的资源符号和相关消息调用点。它们属于依赖核对，不在本工作包的文件覆盖清单中重复计数。

## 确认的问题

### P3（已修正）：排除列表为空时，中文说明把索引规则描述反了

- 位置：src/common/strings.rcinc:482，IDS_NO_EXCLUDED_FOLDERS_DESC。
- 修正前中文为“尚未排除文件夹，它们不会进入全盘索引。”；对应英文在 :1058 为“No folders are excluded from the full-device index.”。
- ui_settings_view.cpp:234 在 settings_index_excluded_paths.empty() 时进入此空状态，:257 使用该说明；index_engine.cpp:430 的 Engine::IsExcludedPath 在排除列表为空时返回 false。
- 因此中文后半句会让用户以为没有添加排除规则的文件夹不会进入全盘索引，与排除语义及英文不一致。此结论只涉及中文文案，不表示所有磁盘或文件实际均被索引。
- 已按明确授权仅改为“尚未从全盘索引中排除任何文件夹。”，保留 ID 和英文。全 src 精确检索确认旧中文仅存在这一处，localization.cpp/runtime_messages.inc 无同文 fallback 需要同步。未通过 GUI 实测显示效果。
- 此文本不在本次 diff 新增范围内，是当前文件全量审查发现的既存问题。

## 已完成的静态核对

用 Python 3.13.15 读取当前工作树文件，并执行下列完整集合核对：

1. string_ids.h：1055 个 IDS 定义，名称重复 0、数字重复 0；范围为 1000–2316。
2. localization.h：1055 个 StringId 成员，成员名重复 0、目标 IDS 重复 0、未定义目标 0；IDS 与枚举目标互相缺失均为 0。
3. strings.rcinc：zh-CN（0x04, 0x02）与 en-US（0x09, 0x01）各 23 个语言表。每种语言有 979 条直接定义，加上 runtime_messages.inc 的 76 条，均为 1055 条。每语言重复键 0、未定义键 0、缺失键 0。
4. 比较每个中英文本按顺序提取的 printf 占位符序列，差异 0。使用的模式为 `%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:I64|ll|l|z|h)?[diuoxXfFeEgGaAcCsSpn])`。它覆盖当前资源中的 %s、%d、%u、%lu、%llu、%zu、浮点格式及 %%；这不是对所有调用者实参类型的编译器级检查。
5. 比较 `{...}` 形式的命名替换占位符：5 组文本包含此类占位符，中英序列差异 0。
6. 直接定义与共享消息均无空字符串；5 个文件无 U+FFFD 替换字符。所有含 IDS 的资源行均可识别为完整字符串定义。
7. 资源行解析接受 RC 的双引号转义 `""`。初版只识别 C 风格引号时曾将 7 个英文搜索消息误报为缺失，已对原文确认并修正解析；最终计数及结论均采用修正后的结果。
8. resource_ids.h 的 47 个资源名称、数字均无重复；app/pulse.rc 恰有 47 个对应 ICON/RCDATA 绑定，双方缺失均为 0；47 个绑定资产路径全部存在。
9. 对 diff 删除的 IDR_FLUENT_CUT_SVG、PASTE、RENAME、DELETE、ADD、OMNIBAR_SEARCH 符号执行 src 范围精确检索，结果为无匹配（rg 退出码 1，属预期）。
10. localization.cpp 的当前 kFirstString / kLastString 为 IDS_SETTINGS / IDS_MATCH_WINDOW_SIZE_HINT，与本次核对得到的 1000–2316 范围一致。新增的 5 个枚举/ID 在两个语言中均有对应文本。
11. 资源字符串与运行时 fallback 通过同一 runtime_messages.inc 宏输入生成；两处资源展开均有对应 #undef，避免该宏泄漏至后续资源定义。
12. 中文修正后重新核对 1055 个键的语言对应、printf 与命名占位符一致性，全部通过。将新字符串按字节替换回旧字符串后计算 SHA-256，恰好等于修正前快照，证明本轮对该文件只改变这一处中文文本，保留原字节编码和换行。`git diff --check -- src/common/strings.rcinc` 通过；Git 提示将来可能执行 LF→CRLF 归一化，不属于本次文本错误。修正后 SHA-256 为 `d9608f6c7e9ea028db3eddcddcfff1d53e894047e84ba65623077dec44e906c8`。

## 维护观察与验证边界

- string_ids.h:703 已结束 PULSE_STRING_IDS_H 包含保护，后续新增定义位于保护外。当前重复包含只会重复定义相同替换文本，本轮未发现实际冲突，不将其升级为已确认功能缺陷；后续整理时可将保护结束移到文件末尾。
- 已阅读 Format 的长度计算、负值返回和输出分配逻辑；本工作包未发现可以据此确认的新增错误。没有逐一审查全仓库的 printf 调用者实参类型。
- 未执行 rc.exe 资源编译或 C++ 构建；资产存在、键集合一致不能替代资源编译、二进制加载或 GUI 验证。
- 未验证中英切换的实际界面、文本截断、窄窗口、DPI、主题、错误状态和无障碍表现；也未对每条说明描述的产品行为做动态验收。静态文案语义审查不能证明这些行为已实现。
- 除上述中文文案问题外，在这 5 个文件及所列交叉核对范围内未发现其他可确认的资源对应或占位符错误。

## 修正前审查快照 SHA-256

| 文件 | SHA-256 |
| --- | --- |
| localization.h | a6d21d47e01abe4536f0a946b214d2ff4a0ecbefecbb98e0d24d8902caeed54c |
| resource_ids.h | 2ffbe263daef40af13916b5bcb2a8d7aea5d5360973e96d76a978fbb41e3ef89 |
| runtime_messages.inc | 9dd35c982778acd45dbdc59cc468135bb6998cbbf692a8ed41f0137e80efc9d1 |
| string_ids.h | ccddbe10202f98609c7f15a9eee3abb131cbbeefb13e31b69c0f682bfdf6c9e0 |
| strings.rcinc | 6d6f54631603c8554a2f1b73194d353d1124f483567f89a6d0f5b88572470a2b |
