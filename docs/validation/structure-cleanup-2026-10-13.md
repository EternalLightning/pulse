# 构建、资源与调度补丁清理（2026-10-13）

## 改动与依据

### 生产源码集合

PULSE_WITH_SELFTEST=OFF 原先只按名字排除部分 app 自测试源文件，另有 7 个文件仍参与普通构建。部分文件虽由预处理条件排空，仍消耗编译调度；部分还在条件块外包含较重头文件。

CMake 改为仅匹配 /app/ 下的 *_test.cpp，关闭自测试时统一排除。ui/ui_hit_test.cpp 是正常 UI 命中测试实现，仍保留。开启自测试仍保留全部 13 个 app 测试文件；未移动文件或关闭测试能力。

### 绘制接口

Render 已返回 bool 以告知是否需要续帧，但 settings_flow_test、settings_interaction_test、search_flow_test 和 address_edit_stability_test 仍手写 void 声明。新增 app_render.h 作为单一声明来源，经 app_internal.h 供调用方共享，移除这些旧声明和动画夹具中的重复声明。编译器现在能够在同一声明约束下检查调用方与实现。

### 调度补丁简化

维护定时器原先先调用 RunUiDeadlines，再调用会重复处理地址搜索截止任务的 TickAddressSearch。现在生产维护路径调用独立的 TickAddressSearchMaintenance；TickAddressSearch 保留组合作用，兼容没有主窗口调度器的独立调用方。

删除仅返回 sidebarFadeDeadline 的 VisualAnimationDeadline 包装及其未使用的时间参数，直接读取既有截止时间状态；调度模块也不再为此包含动画模块头文件。

清除仅写入、无人读取的 AppState::lastFrameTime。清除从未填充、仅在设备和缩放变化时 clear 的 MainRenderer::tray_shadows_。没有改动实际托盘绘制和影子实现。

### 资源清理

删除 assets/fluent 下六个没有当前调用点的旧资源：cut_24_color.svg、clipboard_paste_24_color.svg、rename_24_color.svg、delete_24_color.svg、add_circle_24_color.svg、search_24_color.svg。同步移除 pulse.rc 登记、resource_ids.h 定义、CMake 资源依赖和 CommandIconForResource 中对应映射；剩余 ID 数值保持。

现用工具栏使用 Files 图标；现用搜索图标仍保留 search_visual_24_color.svg。删除文件的 SVG 源内容合计 6,103 字节，不等同于最终 EXE 大小变化。

legacy_icons.h 有字体缺失/LegacyMode 的实际回退调用，保留。缩略图、空状态资源、其他 Fluent 图标、Files 工具栏图标和许可证保留。没有用文件名含 legacy 或存在重复许可证作为删除依据。

## 实际验证

- 主程序 pulse 与 pulse_pane_animation_application_test 构建成功。
- 从当前 CMake 源码选择块提取规则，分别以 PULSE_WITH_SELFTEST=OFF/ON 执行脚本检查：app 测试文件数量分别为 0/13；两种情况下均保留 ui/ui_hit_test.cpp。脚本仅为适配 CMake -P 去掉 CONFIGURE_DEPENDS。
- 用 MSVC /Zs、PULSE_WITH_SELFTEST=1 对四个原有旧 Render 声明调用文件进行语法检查，通过。这不是完整自测试构建或运行。
- 应用级夹具 33 PASS，0 FAIL。新增维护不执行独立防抖任务、组合入口仍消费到期任务两项检查；原有动画、焦点交接状态、真实截止定时器、隐藏停帧、保护状态等检查保持通过。
- 四张主题/DPI PNG 生成成功；查看浅色 100% 窄窗口和深色 150% 宽窗口，现用工具栏、导航及搜索图标未见新增缺失。此前夹具已有的空白图标区域和窄窗口状态截断仍存在；没有声称整个界面完整视觉验收通过。
- 删除资源的 ID/路径静态引用检查无残留；剩余资源 ID 数值与删除前一致；本次受影响已跟踪文件 git diff --check 通过。

## 限制

没有运行完整自测试或发布打包，也未测量构建耗时、程序体积、常驻内存或整机 CPU 的净变化。本轮收益主要是明确减少无用编译输入、无引用资源、重复执行与接口维护负担。没有减少动画采样频率或修改有效缓存预算。
