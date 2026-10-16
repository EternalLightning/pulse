# 文件夹类型否定修复与定向验证（2026-10-16）

## 问题与边界

确认问题：`ParseQuery(L"!type:folder")` / `!kind:folder` 原先生成 `folder=true, file=false, exts=[], ext_not=true`。文件夹种类没有扩展名条件，否定却被写进 `ext_not`，没有转换为“仅文件”。对应中文别名同样受影响。

- 原位置：`src/index/index_query.cpp:381–386`；`ExpandKindToken` 在 `src/index/search_kinds.cpp:76–78` 返回文件夹种类。
- 实际路径：`Engine::Search` 解析后，经 `CollectMatchesLocked` / `MatchQueryNodeLocked` 匹配；普通节点路径和网络/实时匹配跳过空扩展列表，因此错误返回目录；shard base 节点会将空列表的 MatchExt=true 再否定，导致全部排除。此影响来自静态调用链核验，尚未运行 Engine fixture。
- 已实施的最小修复限定于 type/kind 分支的 `folders_only` 处理：正向设 folder，否定设 file，不设置扩展名否定；不改变普通扩展名种类处理，不改变 ExpandKindToken 接口。

## 定向回归

`src/bench/index_test_main.cpp` 添加独立入口 `--folder-kind-negation`，在任何其他 fixture、卷枚举或用户配置访问前直接执行并返回。用真实 `ParseQuery` 和生产 `MatchLiveFilename`，不复制匹配实现。

16 个查询，每个检查解析 flags 和对 PDF 文件、BIN 文件、目录三种 Hit 的命中，共 32 个断言：

- `type:folder` / `kind:folder` 的正向、否定、双重否定。
- 中文 `类型:文件夹` 的正向、否定、双重否定，以及 `!kind:文件夹`。
- `type:document` 的正向/否定，`kind:pdf` 的正向/否定/双重否定，以及 `!ext:pdf`，防止扩展名否定回归。

测试仅使用内存 Hit，不枚举或修改实际文件，不写用户配置。不修改 CMake，不运行历史全套。

## 实际执行记录

测试先于生产修改加入；在串行构建槽开放后，先运行原生产解析器，再实施修复并运行相同 selector。

实际命令：

```powershell
cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_index_test"
.\build\pulse_index_test.exe --folder-kind-negation
```

| 阶段 | 构建 | 定向测试 | 退出码 |
| --- | --- | --- | --- |
| 修复前，原生产解析器 | `pulse_index_test` 成功 | 24 PASS / 8 FAIL | 1 |
| 最小修复后 | 相同目标增量构建成功 | 32 PASS / 0 FAIL | 0 |

修复前失败恰为 case 3、4、8、10，即 `!type:folder`、`!kind:folder`、`!类型:文件夹`、`!kind:文件夹`；每条各失败解析 flags 和真实文件名匹配两项。修复后这些用例全部通过，其他正向、双否定与普通扩展名否定仍通过。

环境记录：最初 sandbox 构建停在 `Re-checking globbed directories...` 且没有后续输出，停止该进程后，常规权限下原命令构建成功；未将该现象认定为产品根因。初次红测使用中文查询作输出标签，wcout 输出提前中断，因此仅将标签改为 ASCII case 编号，生产解析器仍未改，重建并重跑后得到表中完整 32 项红测。正式红测和绿测均使用相同 ASCII 标签测试版本，并在常规权限下执行。

修改仅涉及 `src/index/index_query.cpp`、`src/bench/index_test_main.cpp` 和本记录；无 CMake 修改。定向验证结束后立即释放构建槽，主程序与索引生产目标由主代理统一构建。

## 验证限制

已执行验证覆盖解析器和实时文件名匹配；未运行 Engine shard/base/overlay、网络索引、UI 或全量回归。静态调用链说明与已执行功能结果分别记录；编译成功不代表其他功能验证通过。
