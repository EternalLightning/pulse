# App 大文件补充全文审查（2026-10-16）

范围：从 App 审查代理移交的十个文件，合计 17,784 行审查时快照；先只读审查，随后按主代理授权修复确认问题。定向 fixture 复用主代理已构建的 Pulse 对象，未运行共享构建、不连接真实网络、不修改用户配置。全文阅读不等同于功能验证。

## 精确阅读进度

| 文件 | 审查快照行数 | 已逐段读取范围 |
|---|---:|---|
| src/app/app_main.cpp | 2344 | 1–2344 全文 |
| src/app/app_input.cpp | 3671 | 1–3671 全文 |
| src/app/app_state.h | 774 | 1–774 全文 |
| src/app/app_runtime.cpp | 2315 | 1–2315 全文（输出截断中段 1691–1910 已另次补齐） |
| src/app/app_model.cpp | 1957 | 1–1957 全文 |
| src/app/app_model.h | 558 | 1–558 全文 |
| src/app/app_navigation.cpp | 1917 | 1–1917 全文 |
| src/app/app_commands.cpp | 2016 | 1–2016 全文 |
| src/app/global_search_window.cpp | 714 | 1–714 全文（输出截断缺口 331–430 已另次补齐） |
| src/app/places.cpp | 1518 | 1–1518 全文 |

## 发现

### APP-LARGE-1 [P2] 空标签目录重启复活，损坏标签文件被回退内容覆盖

静态确认：原 `places.cpp:345–368` 仅在 parsed 标签非空时设置 tags_loaded。有效 `{ "tags": [] }` 与损坏/不可读文件均留在 false 分支，随后 EnsureDefaults，且 places_loaded 时 QueueTagSave。DeleteTag（原 1107–1113）允许删除最后标签并只写 tags.json，因此重启会恢复 places.json 旧标签或默认标签，再覆盖空文件。损坏标签文件也会被回退快照覆盖。已移交 App 代理修复，相关新 diff 与运行结果由主代理及 App 代理记录；本补充全文覆盖描述的是修复前快照。

### APP-LARGE-2 [P2] 搜索结果打开所在位置丢失目标选择

原 `app_commands.cpp:523–526` 在 NavigateTo 前设置 pending_selected_name/names/ensure。NavigateTo 调用 StartLoadingPath（app_navigation.cpp:949–952）立即清空这三项，因此不能按操作注释选中原目标；异步列表完成时选择首行。已改为导航后调用已有 SelectLaunchedFile，复用相同目录、加载状态、snapshot 身份及过滤可见性检查，兼容已加载目录中的即时选择。

### APP-LARGE-3 [P2] 分屏输入和绘制使用不同间距

`app_runtime.cpp:364` 原用 4×scale 计算 FocusedPaneRect，但 FillPaneSlots:440 和 app_input.cpp:92 使用 8×scale。后者决定真实绘制/命中区域，前者决定滚动上限、框选、键盘移动和内嵌编辑几何；图标视图临界宽度可出现列数不一致。已将 FocusedPaneRect 对齐 8×scale。未改变实际显示的分屏间距。

### APP-LARGE-4 [P2] 自定义标签颜色命令与第 11 个色块冲突

ShowCreateTagPicker 原颜色基址 30060、自定义颜色命令 30070。七种默认色再添加四种自定义色后，前置颜色范围分支吞掉自定义颜色操作，无法再次打开拾色器。已将固定动作放到色块基址以下（30051），同时将 ShowTagSidebarMenu 的删除命令放到其色块基址以下，消除同类累计色数边界冲突。

### APP-LARGE-5 [P2] 标签菜单跨模态循环保留向量元素指针

ShowTagSidebarMenu 从 places.tags 取得 tag 指针，TrackPopup 后仍用它读取名称。FluentMenu::RunModalLoop（fluent_menu.cpp:1486）派发普通消息；WM_TAG_ADS_DISCOVERED（app_main.cpp:1566）调用 MergeAdsRecords，新增标签可使 tags.push_back 扩容。返回菜单续体后访问旧指针造成未定义行为。已在打开菜单前按值保存名称/颜色；动作提交继续按稳定 TagId 查找，不再解引用跨模态循环的元素指针。

## 原生样式与复用例外

- global_search_window.cpp:234、634 使用共享 CreateChildEdit/HandleChildEditMessage 承载原生 EDIT，保留 IME、输入法组合文本、系统 caret/编辑行为，同时复用应用文字呈现；属于有明确平台职责的原生控件例外。app_main.cpp 的 WM_CTLCOLOREDIT 与 hosted-edit 配合，并非另外实现一套编辑器。
- app_main.cpp:405–410、506–507 将系统图片/目录选择器及属性/打开方式对话框归属 Pulse 窗口；这些是系统/Shell UI，保留原生交互。应用菜单和确认操作使用 FluentMenu / ConfirmDialogSpec，未将系统 UI 人工仿制成应用控件。
- app_runtime.cpp:1–146 的 DetailsMetaWorker 使用进程生命周期实例：安全/卷元数据探测可能长期阻塞，队列只保留最新 pending，窗口 epoch/request id 拒绝陈旧投递。此处不在 UI 线程执行探测，退出不强行等待不可取消的原生探测，是可说明的生命周期例外。
- app_model.cpp 的 FillPaneViewModel/BuildWindowViewModel、app_runtime.cpp 的 FillPaneSlots/BuildVm 集中模型到视图转换；app_input.cpp 复用 renderer 的 ItemRect/VisibleRange/MoveViewIndex 等几何，不新增另一套布局。APP-LARGE-3 修复了其分屏输入边界中的实际复用偏差。
- places.cpp 负责持久化和标签成员索引，app_commands.cpp 通过 QueueTagAds 将 ADS 更新交给 I/O worker；app_navigation.cpp 通过 generation/snapshot 身份隔离异步结果。上述职责边界无需为统一外观搬到绘制或输入路径。

## 验证及限制

新增 `src/bench/app_large_interaction_test.cpp`，复用现有 `scripts/test_integration_fixture.ps1` 的主程序对象链接方式，无 CMake 变更。主代理构建 pulse 后可运行：

```powershell
.\scripts\test_integration_fixture.ps1 -Fixture app_large_interaction_test
```

该定向 fixture 驱动真实 CmdOpenPath 并注入非首行 Unicode 列表；比较三种分屏、三个 DPI 下每个焦点矩形和实际 pane slot；以 11 色色板打开实际创建菜单、触发自定义拾色器后取消；在标签菜单模态循环中强制向量扩容，再继续重命名菜单检查名称快照。配置均 persist=false，列表为隔离注入，不启动服务或真实网络请求。

实际执行上述 fixture 的编译、链接及运行：39 PASS、0 FAIL，`failures=0`，退出码 0。日志：`docs/validation/audit-closure-2026-10-16/app_large_interaction_test.log`。TEMP/TMP 均指定为仓库内 `bench_data/audit-closure-temp`。首次编译缺少 localization 头文件，补齐后首次执行的两项选择断言失败；原因定位到夹具未启动 worker、注入 generation=0 触发了 unsolicited refresh 分支。按已有同类 fixture 的做法显式设置正 generation 后，原断言全部通过，未改动生产选择逻辑或弱化断言。

已完成源码检查、全文阅读和定向交互运行；未截图，不能登记人工视觉验收。菜单和拾色器确实经过模态运行及自动取消，分屏验证为矩形数值一致性，未验证完整 GUI、真实目录加载或远端文件系统行为。未将候选性的高对比度配色、同步驱动/路径探测等观察计作已确认功能缺陷。
