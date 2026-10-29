# Tests A–M 分工范围审查

- 日期：2026-10-30；仅静态只读审查；未改产品源码，未构建或运行测试。
- 原分配：src/bench 下文件名首字母 A–M（不区分大小写），以及 src/app/*_test.cpp、src/app/selftest_1b2.*，共 188 文件、55,574 行。
- 分工调整：bench G–M 已转交 processes/common 分支；尚未读取的 14 个 content/context 文件转交 app integrations 分支。本分支保留已读的 content/context 小夹具，不重复读取其余文件。
- 本分支完整读取：121 文件、34,213 行（15 个 app 测试及 106 个 bench 文件）。覆盖 JSON 与分工后的实际文件集合一致，无重复或漏项。已先查看工作区 diff；跨模块只读取用于核实发现的定向代码段。

## 确认发现

### TA01 [P2] 菜单测试仍把快捷按钮行当成“打开”且把末行当成禁用撤销

- 位置：src/app/selftest_1b2.cpp:1912–1919。
- 证据：同一 TestMenuModel 在 1784–1804 已要求首行 CmdNone + quick_swatches，打开在下一行，末项为 CmdProperties；产品 OrganizeItemMenu（src/app/context_menu.cpp:64–97）也将按钮条排在最前、Properties 排在最后。1913 仍要求从倒数第二行向后跳到 0；1919 仍把第 0 行 command 与 CmdOpen 比较。
- 触发：PULSE_SELFTEST_CASE=pr-shell 或未筛选的 --selftest。
- 影响：当前正确的菜单模型也会被报失败，相关自测失去可靠回归信号。
- 建议：按 command 查找禁用 Undo 和 Open 的实际行，明确覆盖按钮条与普通命令行的不同交互；保留测试，更新过时断言。

### TA02 [P2] 专用设置下拉测试继续点击已移除的窗口效果下拉框

- 位置：src/app/settings_flow_test.cpp:96–124。
- 证据：index 从 0 遍历到 1，要求两个 SettingsDropdown 命中并弹出菜单；但同文件 548 已要求 effect_choice.right == 0。产品 src/ui/settings_layout_sections.h:277–289 明确只保留效果 tile，并把 effect_choice 清空；src/ui/ui_settings_view.cpp:768–770 对 index=0 返回该空矩形。
- 触发：自测构建使用 --test-instance 截图模式，并设置 PULSE_TEST_SETTINGS_FLOW 与 PULSE_TEST_SETTINGS_DROPDOWN（入口 src/app/app_main.cpp:2608–2612）。
- 影响：这个仍可到达的测试分支稳定报告不存在控件的 hit/popup 失败，不能反映实际窗口效果交互。
- 建议：保留语言下拉定位/宽度检查；将 index=0 部分改为现有 SettingsEffect tile 的命中及应用行为。

### TA03 [P2] 实时搜索清空输入断言与退出搜索的新行为相反

- 位置：src/app/selftest_1b2.cpp:1516–1519。
- 证据：要求空输入进入 MakeSearchPath(L"")；产品 SubmitAddressSearch（src/app/address_search.cpp:193–197）会在空白输入时调用 ExitAddressSearch 并返回；src/app/search_flow_test.cpp:739–750 已按恢复 origin 并拒绝晚到结果断言。
- 触发：未筛选的 --selftest 调用 TestLiveAddressSearch（6331）。
- 影响：正确的清空退出行为导致旧自测失败；该检查也没有验证恢复 origin 后的代际取消。
- 建议：改为断言恢复 origin、取消查询及不保存空历史，并验证晚到结果不能覆盖恢复的目录。

### TA04 [P2] “分组启用时单项覆盖”用例实际在已隐藏的分组上检查隐藏

- 位置：src/app/selftest_1b2.cpp:2279–2285。
- 证据：原 prefs 在 2151 初始化；2187–2188 已确认默认隐藏 Share；2191–2198 只在另一个 with_share 对象启用 Share。2279 又对原 prefs 关闭“发送到”，因此即使 SetItemEnabled/单项过滤完全失效，2284 仍可通过。
- 触发：TestContextMenuPrefs（默认 --selftest，6324）。
- 影响：分组启用时单项禁用的回归得不到检测，PASS 含义与标签不符。
- 建议：在同一个 prefs 上启用 Share，先证明发送到可见，再设置单项禁用并证明隐藏。

### TA05 [P2] 自测使用固定系统临时文件名并无条件覆盖和删除

- 位置：src/app/selftest_1b2.cpp:4437–4442、4458–4459；同类路径为 5863–5887。
- 证据：%TEMP%\\pulse_batch_a.txt 与 pulse_batch_b.txt 使用 CREATE_ALWAYS 创建，结束时直接 DeleteFileW；TestRecycleAndBatchRename 已有唯一 suffix（4274–4275），此处未使用。TestUtf8PersistFile 也写入固定 pulse-utf8-persist-test.json。RunSelfTest1B2 与 app_main 的 --selftest 入口没有为 TEMP 建立隔离。
- 触发：默认 --selftest；同名已有临时文件或两个自测进程并发。
- 影响：已有文件被截断后删除；并发运行互相干扰造成不稳定失败。
- 建议：使用本次测试独有目录和 CREATE_NEW，使用 RAII 清理；沿用现成 suffix 也能消除当前冲突。

### TA06 [P2] 上下文动词诊断把外部显示文本再次当成 printf 格式

- 位置：src/app/selftest_1b2.cpp:2377–2379、2391–2395、2412–2414。
- 证据：swprintf_s 将注册表/静态缓存中的 verb.display、verb.verb 写入 line，随后 LogLine(line)；LogLine 在 258–262 对该字符串再次调用 vswprintf_s 且没有额外参数。
- 触发：PULSE_SELFTEST_CASE=context-verbs；动词显示名称/名称含 %s、%n 等格式字符。
- 影响：只读诊断会读不存在的变参、输出乱码或触发 CRT 处理而退出，正好在机器扩展诊断场景失效。
- 建议：LogLine(L"%ls", line)，或提供不解释格式的日志函数。

### TA07 [P2] 流式截图模式仍从磁盘重新读取图片进行像素断言

- 位置：src/bench/file_operation_failure_test.cpp:482–483（Capture 的分支在 99–100）。
- 证据：PULSE_TEST_STREAM_SNAPSHOTS=1 时 Capture 调用 EmitSnapshot 后直接返回，src/bench/snapshot_output.h:19–64 只向 stdout 输出 PNG，不写 image 文件；同一个表达式随即调用 ZigzagPeaksConnected(image, scale)，该函数在 398–399 用 CreateDecoderFromFilename 读取磁盘。
- 触发：开启流式截图并运行 pulse_file_operation_failure_test --motion-only。
- 影响：干净输出目录中实际截图成功也会被判失败；保留旧图片时反而检查旧渲染，可能漏报当前折线连接回归。这个模式原本用于子进程没有文件写权限的环境，因此不能靠事先生成文件规避。
- 建议：直接对 Capture 的本次像素/内存 PNG 做 ZigzagPeaksConnected 检查，再按配置导出或流式输出；保留像素检查，避免读取旧快照。

### TA08 [P2] 删除确认自动化可向其他 Pulse 进程的确认窗口发送按键

- 位置：src/bench/delete_integration_test.cpp:73–83。
- 证据：RejectDialog 的 owner 参数只用于 KillTimer；窗口通过 FindWindowW(L"PulseConfirmWindow", nullptr) 全局查找，未检查进程、线程或 GW_OWNER，随后截图并发送 Enter、Space、Escape 或 WM_CLOSE。该回调同时用于默认用例和 --network-confirmation 分支。
- 触发：运行夹具时，另一个 Pulse 进程已有同类确认窗口，且全局查找先返回它。
- 影响：测试会关闭/操作用户正在处理的确认窗口，可能把其他窗口内容导出为测试截图；真正的夹具对话框未被驱动，还可能一直等待。
- 建议：用 EnumThreadWindows 限定当前测试 UI 线程，并要求 GW_OWNER == owner；截图和按键前再次验证窗口身份。保留确认行为覆盖。

## 覆盖/诊断边界（非删除建议）

- selftest_1b2.cpp:4358–4360 忽略 QueryRecycleBinInfo 的 bool 返回，随后 Check(true) 标记 occupancy probe PASS。应改为 INFO，或以返回值/valid 检查并对环境不可用显式 SKIP；当前不能作为 API 成功的证据。
- search_flow_test.cpp:275–330 的 RunSearchColumnAlignmentTest 产出截图与 regions.txt，并检查几何及保存成功，没有像素比较。当前只确认其为人工视觉检查夹具，未将截图保存成功描述为视觉验收。
- 未确认可删除测试文件。旧断言应修正，不能据此删除整个测试；未以文件年代、缺少自动像素比较或单独目标作为删除依据。d2d_list.cpp 虽为 Stage 0 样例，仍由 CMakeLists.txt:299–300 和 build_d2d.bat:10 提供独立开发入口，docs/build-and-package.md:59 也明确保留其用途。

## 完成状态与验证限制

- 8 个 P2 发现均为当前源码静态证据；未运行测试，不能据此声称实际功能已验证通过。
- 未改产品源码、未删除测试、未构建或运行程序。只写入本报告与 tests-a-m-coverage.json。
- 原 A–M 的其余文件需与 processes/common、app integrations 分支覆盖记录合并评估；转交文件未计入本分支完整读取数量。

