# 搜索、筛选和排序优化验证（2026-10-13）

本轮在已有未提交修改上进行局部优化，未修改压缩、提权、复制进度逻辑。CMake 本轮仅在已有 `model_fixture` 列表新增 `pane_filter`，其他差异属于原有工作。

## 改动与边界

- `src/app/app_model.cpp`：每次筛选只准备一次名称模式；对合适大小的标签目录预先匹配标签名称；借用只读标签索引与不可变目录项，避免逐项复制。名称不匹配时跳过标签路径查询。空目录提前返回；标签数大于目录项数时不预扫描整个标签目录。内容结果筛选同样复用已准备的名称模式。
- `src/app/entry_sort.cpp`、`src/common/known_folder_labels.h`：已知文件夹名称查找采用路径视图或父路径/名称分段比较，减少排序比较器内的路径拼接和规范化分配。特殊路径保留原规范化后备，不改变比较顺序和目录投影算法。
- `src/ui/name_highlight.cpp`：复用一次名称大小写折叠，消除重复子串与拼音匹配；通配符字面片段采用字符串视图，仍使用原通配符匹配器。

未新增长期缓存、后台线程、文件访问或权限放宽。筛选标签表仅在单次调用中存在，大小受标签数及目录项数约束；排序和高亮无需新增每项持久缓存。UI 样式和绘制流程未改。

## 实际验证

经 `scripts\vcvars.bat` 初始化 x64 MSVC 后，定向构建以下目标全部成功：

```text
cmake --build build --target pulse_pane_filter_test pulse
cmake --build build --target pulse_known_folder_labels_test pulse_entry_order_hold_test pulse_drive_view_test
cmake --build build --target pulse_search_enhancement_ui_test
```

执行结果：

| 检查 | 结果 |
| --- | --- |
| `build\pulse_pane_filter_test.exe` | 18,400 组目录/内容筛选对照通过；名称匹配、标签改名、小目录、空/缺失快照检查通过 |
| `build\pulse_known_folder_labels_test.exe` | 已有检查及新增 84 组路径组合通过 |
| `build\pulse_drive_view_test.exe` | 通过 |
| `build\pulse_entry_order_hold_test.exe --directory-contracts-only` | 定向目录投影检查通过 |
| `build\pulse_search_enhancement_ui_test.exe --benchmark` | 74,703 组高亮范围对照与已有高亮/地址布局检查通过 |
| `bench_data\sort_optimization\benchmark.exe` | 30 组完整排序结果一致：5 列 × 2 方向 × 3 文件夹模式，每组 30,000 项 |
| 本轮源码 `git diff --check` | 通过 |

筛选对照保留原实现，覆盖大小写、中文、通配符、多标签 OR、颜色昵称、隐藏/系统文件、UNC 和长路径。高亮对照覆盖重叠匹配、Unicode/代理对、嵌入 NUL、拼音及混合词组。

## 合成基准

同机 Release 构建，时间单位为 ms。测试使用内存数据，不含磁盘枚举、网络访问或完整 UI 绘制。

| 场景 | 原实现 | 优化后 | 规模与取样 |
| --- | ---: | ---: | --- |
| 名称筛选 `report` | 6.641 | 1.254 | 20,000 项，7 次中位数 |
| 通配符筛选 `*.txt` | 4.865 | 1.628 | 同上 |
| 标签筛选 `#work` | 44.711 | 26.982 | 同上 |
| 名称与标签 `report #work` | 45.834 | 12.273 | 同上 |
| 不存在的标签 `#missing` | 222.184 | 0.002 | 同上；预先确认无匹配标签后直接返回 |
| 目录排序：父路径/名称 | 295.11 | 107.31 | 30,000 项，3 次交替顺序中位数 |
| 目录排序：已有完整路径 | 254.80 | 94.67 | 同上 |
| 纯文件排序对照 | 83.34 | 80.83 | 同上；视为波动范围 |
| 普通多词高亮 | 6.85 | 6.24 | 10,000 次计算，总耗时 |
| 拼音多词高亮 | 28.73 | 18.00 | 同上 |
| 短通配符高亮 | 6.56 | 6.36 | 同上；收益较小 |

筛选和高亮可分别用 `build\pulse_pane_filter_test.exe --benchmark`、`build\pulse_search_enhancement_ui_test.exe --benchmark` 复跑。排序对照源码、旧实现副本与可执行文件保存在本地 `bench_data\sort_optimization\`，不是发布负载。

本记录不将编译成功或合成基准视为端到端体验验证。未执行全量回归、真实 SMB/RaiDrive 测试或桌面视觉验收；没有修改安装包或发布版本。
