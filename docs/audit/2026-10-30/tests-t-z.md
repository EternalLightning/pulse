# src/bench t–z 指定文件全文静态审查

审查日期：2026-10-30。范围为父审查分配的 11 个文件，共 3928 行；逐文件全文读取，并检查这些文件的当前 diff。`virtual_clipboard_test.cpp`、`typography_cache_test.cpp`、`thumbnail_pipeline_test.cpp`、`window_material_test.cpp` 属于其他工作包，不纳入本清单。覆盖明细见 `tests-t-z-coverage.json`。

本工作包只写报告与覆盖清单，没有编辑或删除测试/产品源码，没有构建、运行测试、启动 helper/service 或更改用户配置。下述结论来自源码，不代表已进行失败注入或 Windows 运行复现。

## TZ01 / P2：缩略图传输的非空载荷用例允许空载荷通过

- **位置**：`src/bench/thumbnail_cache_test.cpp:91-102`；实际非空输入在 `158-162`（Text/Hex）、`168-174`（一个属性）；空输入另有独立用例 `164-167,175-178`。
- **触发**：响应头与传输完成状态仍正确，但接收/缓存路径丢失 Text/Hex 文本、属性列表，或没有保存响应的 content kind。
- **证据（静态已确认）**：共享成功断言以 `response.text.empty() || response.text == L"ok"` 接受文本，以 `response.properties.empty() || (...)` 接受属性。检查也只在接收结果的 kind 恰好是 Bitmap/Text/Hex 时执行，未把结果 kind 与本次发送的 kind 比较。`payload_ok` 初值为 true，因此返回未失败但空载荷/错误 kind 的结果可以满足 `result_ok`。例如非空 Text 的缓存项变为 `{failed=false, kind=Text, text=""}`，`91-102` 与 `111-112` 的逻辑仍允许其通过；无需假设实际生产代码目前已经发生该错误。生产路径保存文本、属性与 kind 的位置分别是 `src/ui/thumbnail_cache.cpp:535,537-543,561`。
- **影响**：用例名称声称保留载荷，但不能检测预览文字/属性静默丢失；独立的空响应用例与非空响应用例共享过宽判据，削弱这组传输回归保护。
- **建议**：为每个 run 显式传入期望 kind、文本和属性；非空用例严格要求 `L"ok"` 或恰好一个 `ok/ok` 属性，空用例严格要求空集合。Bitmap 同样要求本次预期的 kind、尺寸、stride 与像素，而不是按实际 kind 决定是否检查。
- **定向验收建议（未运行）**：在隔离 fixture 中分别注入“丢文本”“丢属性”“错 kind”并确认对应非空用例失败；真正空文本和空属性用例继续成功。

## TZ02 / P3：动画停止回归会使测试无限循环，无法输出失败

- **位置**：`src/bench/tab_indicator_animation_test.cpp:117-133`，核心为 `123-130`。
- **触发**：Tab 指示器在某个刷新率下不再清除 pending，或停在目标位置后仍报告 pending。
- **证据（静态已确认）**：四种刷新率的逐帧测试使用没有上界的 `for (int frame = 1; ; ++frame)`，唯一退出条件是被测对象的 `!line.pending`。即便连续性判断已变成 false，也不会提前失败，`Check` 只能在循环退出后执行。继续循环还最终会触及 `frame * 1000` 的有符号整数溢出；测试内不存在帧数或模拟时间截止条件。
- **影响**：本应报告的“不能结束动画”回归变为测试挂起，阻塞同一程序中的后续用例；本次没有运行测试，也没有断言当前动画实现已挂起。
- **建议**：按契约动画时长留足余量，设置明确的最大模拟时间/帧数，循环结束后同时检查已停止和终点；超限时输出刷新率、最后边界与 pending 状态。
- **定向验收建议（未运行）**：隔离注入 pending 永不清除，确认各刷新率用例在有限步数内输出失败而非挂起。

## 状态隔离、断言与重复项复核

- `transfer_pipeline_test.cpp` 用 PID/时钟命名独立目录，写入内容并验证源、目标、撤销身份、失败/取消保留及临时文件清理，递归清理前检查绝对父目录与非 symlink（400-420）。它使用真实 CopyFile2 与受控故障注入，不能因存在较底层操作测试而直接判为重复。
- `worker_lifecycle_test.cpp` 与 `worker_projection_cases.h` 使用独立目录和受控 gate。生命周期配额、退休/取消及 watermark，与共享枚举、不同排序订阅、旧缓存回收及 provisional/final 发布覆盖不同契约；没有依据建议删除其中任一组。合成 UNC 请求被有意排在已阻塞 owner 后并在释放前取消；未把字符串包含 UNC 当作访问真实网络的证据。
- `usn_packet_queue_test.cpp` 覆盖队列精确计费、超限、取消和长链清理；其与旧实现的比较是有意展示整包超限语义，不判为无用旧代码。该测试未覆盖 `index_engine.cpp` 同步启动 CatchUpVolume，这个边界对应主 index 报告 IX05，不能把队列测试通过当作启动路径已受限。
- `text_task_reader_test.cpp` 的完整解码对照外，还验证跨块边界、文件版本/路径替换、取消句柄释放、读字节上限与 reader 复用；没有将仅对照同一实现的循环断言判作充分验证。fixture 写在 bench_data，文件通常 delete-on-close，替换场景显式清理旧文件（43-55,350-364）。
- `tab_indicator_ui_test.cpp` 与 `text_render_settings_test.cpp` 的应用 fixture 显式设置 isolatedTest，后者也禁用 searchHistory.persist；其截图输出是人工审阅证据，SaveSnapshot/EndDraw 成功本身不证明无裁剪或视觉效果合格。本次未运行或查看新的截图。
- `window_transfer_regression_test.cpp` 只创建未启动完整主窗口流程的 AppState 和 STATIC 窗口。定向核查 `AppState` 构造（`app_state.h:190-201`）、转移/重绑定（`tab_transfer.cpp:212-315`、`app_runtime.cpp:272-300`）、恢复搜索（`app_navigation.cpp:1123-1181`）：没有在本次调用链确认偏好写入。IndexClient/NetworkAgentClient 默认 running=false；SearchAsync 的入口分别在 `index_client.cpp:425-426`、`network_agent_client.cpp:194-195` 直接返回，因此这里的搜索恢复检查没有启动真实索引连接。不能仅因缺少 persist=false 而报告用户配置污染。
- `titlebar_layout_test.cpp` 本次 diff 仅接入 `surface_scroll_cases.h` 和 `--surface-scroll` 分发；其几何、命中、DPI 与截图职责没有发现足以登记的独立问题。关联 header 全文审查归其他工作包，本清单不虚增其覆盖。

其他工作包拥有 shared_settings/realtime_search 的真实 content-index.config 隔离问题（TB01），本报告不重复编号。未发现足以要求删除整文件的过时测试；建议先收紧 TZ01 和增加 TZ02 的失败边界，保留现有独立契约覆盖。

## 实际验证与限制

实际完成：11/11 文件全文读取、当前 diff 审阅、必要调用链静态确认、覆盖 JSON 的精确集合/唯一性/行数核对，以及报告格式差异检查。没有构建、执行测试或 helper/service，没有进行运行时性能、GUI、DPI、截图视觉验收。
