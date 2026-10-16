# 查询解析、拼音辅助索引及结果合并全文审查（2026-10-16）

本轮先静态审查，再经主代理明确授权，仅修复缓存缩小条件并增加定向回归。先检查本次 diff：保留 `type/kind` 的目录取反修复和 `MergeSearchResults` 的预计算排名/Index 顺序快速路径；以下问题均在本轮开始时已存在，未声称由该 diff 引入。阅读范围与问题定位行号记录修复前快照；本轮删除 6 行旧辅助函数后，其后的行号相应变化。

## 阅读范围

完整逐段阅读以下当前工作区文件：

| 文件 | 全文范围 | 关注点 |
| --- | --- | --- |
| `src/index/index_query.cpp` | 1–757 | token 生命周期、引号/否定/OR、名称/扩展名/类型/大小/日期/路径解析、匹配、排名及缓存缩小条件 |
| `src/index/index_query.h` | 1–83 | 编译后查询及 Term 的字段语义与接口 |
| `src/index/index_engine_pinyin.cpp` | 1–163 | sidecar 身份/长度/顺序/校验、原子替换、线程代际、映射生命周期及发布 |
| `src/index/search_result_merge.cpp` | 1–80 | 元数据合并、分页边界、Index 直接复制、排名预计算、比较器与借用指针生命周期 |
| `src/index/search_result_merge.h` | 1–9 | 合并接口和所有权 |

辅助定位阅读（不计全文覆盖）：`index_engine.cpp:561–588,872–935,986–1050,1325–1405,2025–2075`；`index_engine.h:436–472`；`network_index.cpp:400–443`；`filename_pinyin.cpp:80–150`；`index_test_main.cpp` 的查询相关检索命中及其解析断言。未全文阅读上述辅助文件。已有 merge 优化历史仅用于识别保留边界，历史测试结果不计本轮验证。

## 可操作缺陷

### P1：查询文本追加可能扩大结果，缓存却只在旧匹配集合内搜索

状态：已修复并完成下述定向 red/green 验证。

位置：`index_query.cpp:672–683`；使用处 `index_engine.cpp:1360–1372`。

`QueryCanNarrow` 对文本前缀、OR/否定符数量及拼音转换作检查，但没有证明编译后新谓词是旧谓词的子集。普通合法输入 `size:<1` 追加为 `size:<10` 会通过全部检查，新查询应加入 1–9 字节文件，却通过 `NarrowMatchesLocked` 仅检查旧集合；`ext:txt` 追加为 `ext:txt;pdf` 同样会永久漏掉原来不在集合内的 PDF，直到触发一次完整收集。两个例子的 scope、universe 和 epoch 可以保持不变，调用端的额外 guard 不能阻止该路径。

相同逻辑还需检查扩展排除、非拼音否定名称（例如 `!1`→`!12`）、带通配符的追加与引号变化。建议缩小优化只接受已证明单调的编译后条件；保守做法是先限定为相同模式的正向普通名称子串扩展，其他情况完整收集。回归验证应比较同一 Engine 热查询与新 Engine/失效缓存冷查询的完整结果集合，至少覆盖上面两个合法输入序列；不能只断言 `QueryCanNarrow` 返回值。

### P2：有名称值的 file/folder 否定没有实现整个条件取反

状态：明确待处理，本轮未修改，需独立评估语法后修复。

位置：`index_query.cpp:362–373`；消费处 `index_engine.cpp:886–888,905–925,2037–2069`。

`!file:foo` 设置 `file=true` 并解析 foo，但没有使用 `negated`，因此与 `file:foo` 完全同义，仍返回名称含 foo 的文件。`!folder:foo` 设置 `folder=true,name_not=true`，目录类型在名称取反前已经强制检查，因此仅返回名称不含 foo 的目录，所有文件都会被排除；这与该分支自己的 `not (folder named foo)` 注释不符。裸 `!file:` / `!folder:` 的类型切换已有专门分支，不受这个有值问题影响。

建议将类型加名称条件作为整体可取反谓词，或者明确定义并一致实现产品语法；至少不能让 `!file:foo` 忽略 `!`。回归 fixture 应含 foo 文件、其他文件、foo 目录、其他目录四种对象，分别检验两个带值否定与两个裸类型否定，且覆盖不同查询存储分支。

### P2：跨目录组件的非通配符路径条件不能命中，反向路径条件失效

状态：明确待处理，本轮未修改，需独立评估路径语义后修复。

位置：`index_query.cpp:322–329,375–379,394–398`；匹配处 `index_engine.cpp:907–919,2042–2057` 和 `network_index.cpp:408–427`。

解析器把相对 `path:foo\\bar`、裸 `foo\\bar`、反向绝对 `!path:C:\\skip` 以及第二个绝对 path 条件保留为 `name_in_path` 的 Substring Term。当前非 Wildcard 路径匹配却把路径拆成单个名称组件，再对每个组件执行包含匹配。没有任何单个组件包含分隔符，所以正向条件永不匹配，反向条件则永远成功，无法排除 `C:\\skip` 下的内容。首次正向绝对 `path:` 被提升到 `path_prefix`，有独立解析/定位分支，不属于此问题。

现有 `index_test_main.cpp:306–324` 只断言额外及反向绝对 path 被保留为过滤项，没有断言结果。建议含分隔符的条件对规范化后的完整路径匹配，保留无分隔符条件的组件匹配语义；须按目录边界决定绝对路径排除是否允许 `skip2` 命中，避免未经产品定义直接改成宽泛前缀。定向 fixture 应含 `C:\\skip\\a.txt`、`C:\\skip2\\a.txt`、`C:\\foo\\bar\\a.txt` 和非相关目录，并分别核验 local/network 的结果。

## 其余结论与边界

- 结果合并的 RankedHit/候选指针仅借用 const 输入容器，函数内没有修改输入且返回逐项复制的 Hit；未发现正常输入下的悬空引用。`end = begin + min(limit, count - begin)` 避免旧式 `begin + limit` 溢出；Index 快速路径的跨 local/network 下标与该分页界限一致。排名预计算保留原排名和名称/路径 tie 规则。没有以静态检查替代历史 oracle 或真实跨后端排序功能验证。
- 拼音构建在 shared mutex 下读取映射，发布时重新检查 request 代际和映射身份；Stop 在关闭 map 之前 join 拼音线程。正常生命周期中没有发现映射释放后使用的证据。pair mask 的生产函数只设置 26×26 位，对应 676 个 posting，末 word 的保留位不会进入构建下标。
- sidecar 验证身份、每项数量上限、严格递增且非负的 ID、校验值和尾部 EOF；失败会回退重建。校验不是针对恶意可改写文件的真实性保证；本轮没有故障注入、sidecar 篡改或多进程同时写入验证。构建/sidecar I/O 持有 shared mutex，可能延迟需要独占锁的更新；没有测量延迟，因此仅记录性能/取消边界，不上升为已复现挂起。
- 数值输入中的 `ParseU64` 累乘和单位乘法没有溢出检测（`index_query.cpp:67–96`）；无效日期也可能退化为零边界。这些需要超大数值或无效日期输入，不作为本轮普通合法输入的优先缺陷；建议后续输入验证统一处理，不能据此宣称内存越界。

## 实施与实际验证

`index_query.cpp` 的 `QueryCanNarrow` 现在要求前后编译结果均为单个正向普通名称 Substring 条件、没有路径前缀或内容谓词，名称保持前缀扩展且拼音总开关不变；继续拒绝从普通匹配转为拼音匹配的扩展，保留已有拼音匹配的安全扩展。扩展名、范围、否定、精确名称及其他复杂语法回退完整收集。代价是复杂查询键入不再使用旧集合缩小优化；普通正向名称键入仍可使用。删除仅旧启发式使用的 `CountChar`，保留已有目录种类取反修复。

在 `index_engine_test.cpp` 新增独立 `--query-narrowing-only` selector。通过 `AddForTest` 构建两个独立内存 Engine，比较热查询和冷查询的 total、完整路径集合及必需命中，覆盖大小上限扩大、扩展名备选追加、扩展名排除延伸、名称排除延伸、普通名称延伸、已有拼音延伸和首次启用拼音；再检查普通/拼音延伸保留优化，以及首次启用拼音、引号精确名称延伸和 nopinyin 控制切换回退收集。

- 修复前：仅构建 `pulse_index_engine_test`，退出 0；normal 权限的 `pulse_index_engine_test.exe --query-narrowing-only` 为 6 FAIL、6 PASS、退出 1，四个热/冷集合缺陷全部复现，另两个 gating 断言失败。
- 修复后：同目标增量构建退出 0；同 selector 为 12 PASS、退出 0。没有运行其他 selector 或整套回归。
- 默认 sandbox 首次启动测试退出 1 且没有输出；相同二进制在授权 normal 权限运行后获得上述可诊断 red 输出。构建因本会话已有 sandbox VerifyGlobs 停滞证据直接使用授权 normal 权限，未把无输出启动失败作为产品逻辑证据。
- `git diff --check` 对 `index_query.cpp` 与 `index_engine_test.cpp` 通过。构建包含并发分工的已落盘改动，本 selector 只验证本轮查询行为，不替其他分工宣称验证通过。

未执行真实磁盘映射索引、网络 fixture、UI、性能测量或并发压力测试。另两项 P2 尚只有静态调用链证据，本轮未修复、未动态验证。
