# 小批次索引辅助模块审查（2026-10-16）

本轮先读取仓库 AGENTS.md，再检查指定八文件的工作区 diff（初始均无差异），全文阅读这些文件，仅定向阅读必要调用者及既有测试。第一阶段只读；主审复核并授权后，仅实施 H1 修复及其隔离用例，H2 保留报告。未运行服务或高权限操作，不提交。其他并行工作的未提交内容不属于本报告结论。

## 已确认的静态缺陷

本节行号对应本轮初始审查快照；修复后的当前定位见后文符号名。

### H1 / P1：迁移前递归改写普通资料的 ACL，后续失败也不恢复

- 位置：`src/index/index_path_service.cpp:73-79`，尤其 77 行；实际修改来自 `src/index/index_raw_storage_security.h:28-50`。后置路径和冲突检查位于 `src/index/index_migration.cpp:173-191`。
- 触发：管理员通过 `--set-index-path` 选中已有资料目录，或选中旧索引目录的祖先/后代，或目标含冲突索引。`ConfigureServiceIndexPath` 在迁移预检、同路径判断及打开服务前就调用 `ProtectIndexRawTree(previous.index_path)` 和 `ProtectIndexRawTree(target)`。
- 结果：保护函数递归遍历所有子项，在 38-39 行把 owner 和受保护 DACL 改为管理员/SYSTEM，未筛选索引 payload。普通文件仍存在但普通用户可能失去访问权限。目标后续因重叠、索引冲突或 SCM 失败而拒绝时，也没有恢复这些安全描述符。
- 源侧同样成立：即使选择合法空目标，源 root 的普通资料也被改 ACL。既有 `src/bench/index_migration_test.cpp:38-40,66-68` 明确构造并要求保留 `config.json`、`my-file.txt`、shard 内的 `my-notes.txt`；因此不能假定源 root 只含索引数据。
- 已有防护：目标祖先/当前对象的 reparse 检查、复制哈希、拒绝重叠、拒绝不同目标索引、回滚已复制索引。这些防护没有回滚 ACL，而且生效次序晚于递归保护。
- 最小复现设计（本轮未执行）：隔离 source 放一个合法索引 artifact；target 放普通 `notes.txt` 或选 source 下的 nested；注入 protect 回调记录调用和路径。当前顺序会先记录 source/target 保护，再在 Copy 校验中拒绝。另对 mixed source 中 notes 的 ACL 做只读前后比较即可验证源侧影响；不应对用户目录执行。
- 最小修复方向：建立无副作用迁移预检，在创建目录、ACL 改写、SCM 操作前验证本地绝对路径、重叠、祖先 reparse 和目标内容；拒绝目标普通资料。目标通过后先保护再复制，不能把保护后移到复制完成之后。源保护必须限定为已知 artifact 和必要目录，保留 mixed source 的普通资料权限语义。Copy 内保留重复预检以处理状态变化。
- 建议验证接缝：在 migration 模块提供可注入 protect 回调的准备包装器，记录非法/重叠/普通资料/冲突目标为零次回调，合法空目录则 protect 在首次 copy 前。可用现有 `pulse_index_migration_test`，涉及 `index_migration.h/.cpp`、`index_path_service.cpp` 和现有测试文件；不需要 SCM/UAC，不必新增 CMake 目标。
- 证据等级：调用顺序和递归写 ACL 可由代码证明；本轮没有执行 Windows ACL 或真实服务复现。

### H2 / P2：否定的文件夹种类被当作正向目录过滤

- 位置：`src/index/search_kinds.cpp:72-78` 返回 `folders_only=true`；调用者 `src/index/index_query.cpp:381-386` 对否定 token 仍设置 `t.folder=true`，只把 `negated` 写入 `ext_not`。
- 触发：查询 `!type:folder`、`!kind:folder` 或对应 folder 别名；解析结果为 `folder=true, file=false, exts.empty(), ext_not=true`。
- 结果：`src/index/index_engine.cpp:887-892` 的常规匹配首先拒绝所有文件，且空 exts 不应用 ext_not，故反而保留目录。另一路 `index_engine.cpp:2060-2068` 先对空扩展名过滤取反，再正向要求目录，可能全部拒绝。两者均没有表达“排除目录”。
- 已有防护：`index_query.cpp:363-366` 对 `!folder:` 有显式 `folder=false,file=true` 转换，但 type/kind 分支没有复用。
- 最小复现：只解析 `ParseQuery(L"!type:folder")` 即可观察错误 flags；进一步在一个目录和一个文件的隔离 Engine fixture 查询，应仅返回文件。对 `type:folder` 正向及 `!kind:文件夹` 同时检查。
- 最小修复：type/kind 分支对 folders_only 单独处理否定，设 `t.file=negated; t.folder=!negated`，且不把 folder 否定塞入 ext_not；保持普通扩展名种类的否定逻辑。
- 证据等级：解析及匹配调用链已读，未构建/执行 fixture；缺陷修复点位于目标模块的必要调用者，不建议把 ExpandKindToken 强行变成感知 negation 的接口。

## 静态风险与未确认事项

- `index_caller_visibility.h:43-53,63-80`：祖先按路径逐个授权后关闭句柄，再以完整路径、服务身份读下层 descriptor；祖先身份变化到下一次 open 之间仍存在竞态窗口。最终对象 reparse 拒绝、caller impersonation open、directory identity/descriptor 最终重验显著限制披露，但不能仅据这些代码证明中间完整路径打开不会暂时穿过被替换的祖先。未构造并发 junction/rename fixture，列为静态风险，不定为可利用漏洞。
- `index_caller_visibility.h:20-32`：ValidateDirectories 没有每项 deadline 检查，且系统调用不能被 budget 直接中止；2 秒预算限制授权循环，不能证明端到端处理不超过 2 秒。未做延迟注入，不将其描述为已验证超时故障。
- `index_visibility.cpp:7-10`：除 node_modules 外，特殊目录名称使用大小写敏感比较。大小写变体不会命中此排除规则；是否要求 Windows 目录排除大小写无关，尚无本轮产品规则依据，因此仅记录规则一致性风险。该排除不是 ACL 安全边界。
- `index_path_service.cpp:101,116`：restart 只约束失败回滚路径，正常迁移会启动原来停止的服务。代码行为确定，但未找到“保留停止状态”的要求，不按缺陷计。

## 逐文件覆盖

| 文件 | 完整阅读范围 | 检查结果 |
| --- | --- | --- |
| `filename_pinyin.cpp` | 1-154 | UTF-16 surrogate 读取、未映射字符、DP offset/count、组合索引、候选字母与 pair 掩码均已检查。当前静态数据满足索引与小写字母前提，未发现当前数据导致的越界。末尾半音节匹配由既有测试明确支持。 |
| `filename_pinyin.h` | 1-20 | UTF-16 span、11 个 uint64 pair 容量覆盖 676 pairs、版本与接口契约。未确认新缺陷。 |
| `index_path_service.cpp` | 1-129 | 服务句柄 RAII、超时/状态、停止/复制/配置/失败回滚与 ACL 顺序；确认 H1。未重复 index_shard 状态审查。 |
| `index_path_service.h` | 1-7 | 调用约束明确为 elevated helper，非 UI；未确认新缺陷。 |
| `index_visibility.cpp` | 1-65 | base child_order 与 mutable overlay、迁移父节点检查、visited 去重、hidden 传播及 delta Meta 写回。未确认越界/环路新缺陷；名称大小写见风险。 |
| `index_caller_visibility.h` | 1-124 | NUL/冒号/远程语法/点段拒绝、slash 规范化、逐祖先授权、descriptor/reparse/identity、restricted token 与 impersonation、缓存上限、最终目录重验、Deleted/MovedOut 拒绝均已读。风险见上；未证明新 ACL 绕过。 |
| `search_kinds.cpp` | 1-94 | 内置种类、未知 raw 作为自定义扩展名、点前缀、分隔符、folder 输出。确认必要调用者的否定语义错误 H2。未把扩展名列表取舍当作缺陷。 |
| `search_kinds.h` | 1-27 | enum/接口/扩展名约定已查；未确认接口自身新缺陷。 |

## 实际验证和边界

实际完成：八文件 diff 检查及全文静态审查；必要调用者、既有 migration/pinyin/security fixture 定向阅读；PowerShell 只读解析当前 `third_party/ib-pinyin-cpp/ascii_data.inc`，确认 1514 条 reading 均为非空 a-z（额外匹配的一条 `P(s)` 是宏形参，不计数据）、1104 组 combination 的 n 与 initializer 长度一致且不超过 10，最大 reading index 为 1512；7 个 range 的表项长度分别为 27630、53271、1、80、1、1、2，与闭区间一致；table 中非 F 值均小于 2618。没有把硬编码 1514/1104 当作当前越界：其与当前 data 定义一致。

本轮未执行服务状态迁移、真实 ACL 写入、并发路径替换或 UI。后续授权实施的构建和回调 fixture 结果见下；回调用例曾在常规 sandbox 中无输出退出，因此以相同命令在放宽执行限制环境中的实际输出来报告结果。只读数据检查不覆盖未来第三方数据升级，现有硬编码常量和无字符过滤的位移表达式依赖该数据保持约定。

## 主审授权后的 H1 修复及定向验证

- `index_migration.h/.cpp` 新增 `PrepareServiceIndexMigration`：先完成只读源/目标边界及目标完整枚举；目标只能是专用空目录或含与源匹配的已识别 artifact。拒绝相对路径、UNC、远程盘、源根目录、ADS/NUL、非法字符/设备名、尾部点或空格导致的 Win32 路径别名，以及包含关系。盘根目标继续按原约定映射到 Index 子目录。所有已知准入检查均在任何保护回调或建目录前完成。同路径必须先确认不是普通文件，再返回成功且不修改 ACL。
- 准入后只对源 Inventory 识别的普通 artifact 文件调用保护回调，源目录和混合资料不保护；然后创建并保护专用目标，目标复制发生在保护之后。
- `index_raw_storage_security.h` 新增按句柄的 `ProtectIndexRawFile`，拒绝目录和 reparse，以避免源父目录 DACL 的继承传播。
- `index_path_service.cpp` 用该准备流程替换旧递归保护顺序；随后现有停服务、复制、配置、启动和回滚流程保留。`CopyIndexForMigration` 本身的用户级混合目录语义保留。
- `index_migration_test.cpp` 增加独立 `--service-prepare` selector 和注入保护回调的隔离用例，覆盖普通资料、嵌套/祖先、相对/UNC、路径别名、索引冲突、普通空目录和 shard 内普通文件在准入失败时零回调；合法 mixed source 只保护三个 artifact 文件；目标保护后才复制；匹配目标准入、同路径无 ACL 动作、同路径普通文件拒绝、源/目标保护失败停止。
- 原服务流程缺少注入点且会执行高权限 ACL/SCM，旧缺陷以静态顺序证据证明，没有执行旧实现 ACL 写入。新回调用例不宣称验证真实 DACL 或服务生命周期。
- 剩余限制：准备阶段和执行阶段之间仍有按路径操作的竞态；本次没有引入全程目录句柄锚定或并发重解析点攻击 fixture。失败保护可能已改变部分索引 artifact ACL，但不会通过准备流程故意遍历普通源资料或在已知非法目标上调用保护。

实际执行结果：

| 命令/检查 | 结果 |
| --- | --- |
| `cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_index_migration_test pulse_index"` | 在 require_escalated 执行环境完成，exit 0；两个指定目标构建通过，无新增编译警告。未运行全量构建。 |
| `.\build\pulse_index_migration_test.exe --service-prepare`（普通 sandbox） | exit 1、无输出；原因未判定，不记作产品失败或通过。 |
| 相同 selector 命令（require_escalated） | exit 0，18 PASS / 0 FAIL。使用 fake ACL 回调和项目 `bench_data` 的进程独立 fixture，无真实 ACL/服务操作；未运行原迁移完整套件。 |
| 本次代码文件 `git diff --check` | 通过；Git 仅提示 `index_raw_storage_security.h` 后续可能进行 LF/CRLF 转换，无空白错误。 |

这些用例验证已知非法输入先于 ACL 回调拒绝，以及准备/保护/复制控制流；不保证未来任意磁盘 I/O 失败都没有副作用，也不等同于真实高权限服务迁移或安全描述符继承的动态验证。H2 由主审另行安排查询模块代理复核与处理，未在本文件组修改。
