# 双拼索引搜索（方案）

Pulse 当前的拼音搜索支持**全拼**与**首字母**两种输入（`src/index/filename_pinyin.cpp`），不支持**双拼**。本文给出双拼落地方案与取舍，暂未实现。

## 1. 现状

| 能力 | 状态 | 位置 |
| --- | --- | --- |
| 全拼 `中国报告 ← zhongguo` | 支持 | `filename_pinyin.cpp:45` `Match(..., initials=false)` |
| 首字母 `中国报告.txt ← zgbg` | 支持 | 同上 `initials=true`（`:65`） |
| 全拼/首字母混合 `拼音搜索.txt ← pysousuo` | 支持 | `src/bench/filename_pinyin_test.cpp:26` |
| 多音字（不展开笛卡尔积） | 支持 | `Readings()` `filename_pinyin.cpp:34` |
| 双拼（小鹤/微软/自然码…） | **不支持** | — |

读音数据：`third_party/ib-pinyin-cpp/ascii_data.inc`，无声调 ASCII 全拼（如 `zhong`）。

## 2. 关键难点：候选集预过滤

`src/index/index_engine.cpp:1007` 起的拼音候选路径：候选集取 `pinyin_chinese_ids_`（含中文的全部节点），再用查询中**相邻字母 bigram 的最小倒排表**收窄：

```cpp
const std::vector<int32_t>* candidate_ids = &pinyin_chinese_ids_;
for (size_t i = 1; i < term.name.size(); ++i) {
    const wchar_t a = FoldChar(term.name[i - 1]), b = FoldChar(term.name[i]);
    if (a < L'a' || a > L'z' || b < L'a' || b > L'z') continue;
    const auto& posting = pinyin_pair_ids_[(a - L'a') * 26 + b - L'a'];
    if (posting.size() < candidate_ids->size()) candidate_ids = &posting;
}
```

倒排表由**全拼**生成（`PinyinCandidatePairs()`，676 条 posting），落盘于 `<index>.pinyin-v2` sidecar，identity 含 `(kPinyinSidecarVersion=2 << 32) | kPinyinDataVersion=1`（`index_engine_pinyin.cpp:7`、`:17`）。

小鹤把「双拼」打成 `ulpb`，bigram 为 `ul`/`lp`/`pb`，在全拼图中近乎不存在 → 被选为「最小 posting」→ 候选集几乎为空。**即使匹配算法改好，搜索也会表现为完全无效。**这是双拼落地的真正门槛。

补充：`PinyinCandidateLetters()`（26-bit 首字母掩码）在该查询路径上未被使用，只有 pair posting 参与剪枝。

### 取舍（已定）

- **A（先做）**：双拼查询跳过 bigram 收窄，`candidate_ids` 恒为 `pinyin_chinese_ids_`。
  `kPinyinDataVersion` 不变，sidecar 不失效，**用户无需重建索引**；代价是双拼查询扫描全部含中文节点。
- **B（后评估）**：把双拼 bigram 写入 sidecar，`kPinyinDataVersion` 1→2，**全量重建**（见 `index-migration.md`），换方案需再次重建，posting 变密、剪枝率下降。

**结论：先实现 A，用 `bench_data` 量化双拼查询延迟；只有在大库上明显退化时再评估 B。**

## 3. 实现方案

### 3.1 新增 `src/index/shuangpin_scheme.h/.cpp`

```cpp
enum class ShuangpinScheme : uint8_t { Off, Xiaohe, Microsoft, Ziranma, Sogou, AbcInput, Ziguang, PinyinJiajia };
ShuangpinScheme ShuangpinSchemeFromKey(std::string_view key);  // "xiaohe" -> Xiaohe
std::string_view ShuangpinSchemeKey(ShuangpinScheme scheme);
// "zhuang" -> {'v','l'}（小鹤）；非法音节或方案无映射时返回 false
bool ShuangpinEncode(ShuangpinScheme scheme, std::string_view reading, char keys[2]);
```

按**书写拼写**切分音节：先匹配 `zh/ch/sh`，否则首辅音（含 `y`/`w`）为声母，其余为韵母；零声母单独处理。键位表为 `constexpr` 数组，增删方案只改数据。

小鹤韵母键位（与口诀「秋闱软月云梳翅／松拥／黛粉／更航安／快莺／两望／奏／夏蛙／撇／草／追鱼／滨鸟眠」交叉核对）：

| 键 | 韵母 | 键 | 韵母 | 键 | 韵母 |
| --- | --- | --- | --- | --- | --- |
| q | iu | a | a | z | ou |
| w | ei | s | ong, iong | x | ia, ua |
| e | e | d | ai | c | ao |
| r | uan, üan, er | f | en | v | ui, ü |
| t | ue, üe | g | eng | b | in |
| y | un, ün | h | ang | n | iao |
| u | u | j | an | m | ian |
| i | i | k | uai, ing | o | uo, o |
| p | ie | l | iang, uang | | |

声母 `zh→v`、`ch→i`、`sh→u`，其余同全拼首字母。零声母（小鹤）：单字母韵母＝韵母首字母＋韵母键（`啊 aa`、`哦 oo`、`额 ee`）；双字母韵母保持全拼（`爱 ai`、`恩 en`、`欧 ou`、`儿 er`）；三字母韵母＝首字母＋韵母键（`昂 ah`）。

微软／搜狗使用同一张表（零声母固定以 `o` 起键，`ing` 在 `;` 键）。自然码、智能ABC、紫光、拼音加加建议对照 RIME `double_pinyin_*.yaml` 录入，不凭记忆手写。

### 3.2 匹配层 `filename_pinyin.h/.cpp`

- `PinyinMatchKind` 增加 `Shuangpin`，排序权重介于 `Full` 与 `Initials` 之间（参考 `filename_pinyin_test.cpp:41`、`:47` 的打分断言）。
- `Match()` 的 `bool initials` 改为 `enum class MatchMode { Full, Shuangpin, Initials }`，在 `Readings()` 回调中新增：

```cpp
if (mode == MatchMode::Shuangpin) {
    char k[2];
    if (query.size() - offset >= 2 && ShuangpinEncode(scheme, reading, k) &&
        FoldChar(query[offset])     == static_cast<wchar_t>(k[0]) &&
        FoldChar(query[offset + 1]) == static_cast<wchar_t>(k[1]))
        advance(offset, 2);
    return;  // 双拼轮次不同时接受全拼
}
```

DP 结构不变：仍是每字一次状态转移、O(查询长度) 空间、多音字不展开。

- `FindPinyinMatch()` 增加参数 `ShuangpinScheme scheme = ShuangpinScheme::Off`，顺序 `Full → Shuangpin → Initials`（避免双拼抢走 `zg` 这类首字母查询）。`PinyinEligible()` 不变。

### 3.3 设置与串联

- `src/app/app_prefs.h/.cpp`：新增 `search.shuangpin_scheme`（`off|xiaohe|microsoft|sogou|ziranma|abc|ziguang|jiajia`，默认 `off`）。
- `src/ui/ui_settings_view.cpp`、`ui_settings_core.cpp`：搜索分组下拉框；文案见 `src/common/localization.h`。
- 透传：`src/app/search_query.cpp`、`src/app/global_search_controller.cpp` → `src/index/index_query.cpp:541`、`:658`、`:702` → 高亮 `src/ui/name_highlight.cpp:57`（不同步会出现命中但不高亮）。
- `index_query.cpp:702` 处（`term.pinyin = term.name_how == NameHow::Substring && PinyinEligible(term.name)`）旁增 `term.shuangpin`；`index_engine.cpp:1010` 在 `term.shuangpin` 时跳过 bigram 收窄（A 路线）。
- 注意：微软／搜狗的 `ing` 落在 `;` 键，需确认 `search_query.cpp` 的分词不会把 `;` 当分隔符。

### 3.4 测试

- `src/bench/filename_pinyin_test.cpp`：`中国报告.txt ← vsgobcgc`、`双拼.txt ← ulpb`、`小鹤双拼.txt ← xnhe`、`除数.txt ← iuuu`、零声母、`scheme=off` 时 `vsgo` 不得命中、小鹤键在微软方案下不得命中。
- `src/bench/index_pinyin_test.cpp`：快照搜索链路 + 多音字（`重庆 ← isqk` / `← vsqk`）。
- `src/bench/search_enhancement_ui_test.cpp`：高亮范围覆盖原汉字跨度。
- 性能：`bench_data` 对比开/关双拼的全盘查询耗时，作为是否上 B 路线的依据。

### 3.5 工作量

约 500–700 行（键位表约占一半），2 个新文件 + 8 个改动文件。实现期间只构建受影响的索引、主程序和相关测试目标；需要更新完整交付文件时使用 `.\build_release.bat`。目录及命令见[构建与打包指南](build-and-package.md)。

## 4. 原型验证结论

已按上述设计在仓库外实现 `ShuangpinEncode` 与带 `MatchMode::Shuangpin` 的 DP（stub 读音表）并编译运行，全部用例通过：

- 小鹤编码：`zhong→vs`、`guo→go`、`shuang→ul`、`pin→pb`、`xiao→xn`、`chu→iu`、`shu→uu`、`bao→bc`、`ai→ai`、`ang→ah`、`a→aa`、`er→er`、`nv→nv`、`yue→yt`、`yun→yy`、`yuan→yr`、`qing→qk`、`wang→wh`
- 微软编码：`shuang→ud`、`pin→pn`、`xiao→xc`、`ai→ol`、`ang→oh`、`qing→q;`
- 匹配与回归：双拼命中且高亮跨度正确；`zhongguo` 仍判 `Full`、`zgbg` 仍判 `Initials`、`pysousuo` 仍命中；`scheme=off` 无误报。
