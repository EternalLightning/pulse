# GitHub bug issue 与本地代码核验报告

日期：2026-10-23（Asia/Shanghai）。仓库：[jimmgreen/pulse](https://github.com/jimmgreen/pulse/issues)。本地：`E:/Projects/pulse`。本文是唯一完整报告，合并原文筛选、本地核验、已确认子问题修复及微信接管检查。结构化 JSON 和日志为证据包，不另作报告。

## 1.2.1.0 持久合成编辑正式发布（2026-10-24）

**当前状态：用户已手动认可持久合成原型，授权正式引入、升级为 1.2.1.0 并替换安装版。本轮已完成。**

- 正式行为：移除生产代码的 `PULSE_TEST_COMPOSED_EDIT` 门控，透明合成宿主默认创建持久编辑表面；继续保留呈现不可用或失败时的原生回退。文字自动、清晰、平滑三选项保留。版本单一来源为 `version.txt=1.2.1.0`。
- 定向验证：子编辑框自动／清晰／平滑各 153 PASS，真实高级搜索和批量重命名对话框各模式 425 PASS，文字设置 493 PASS，合计 2227 PASS / 0 FAIL。子编辑框检查含持续合成标记、键盘不切换原生 redraw、隐藏／显示、销毁、呈现失败后回退及 Unicode 编辑。
- 保留失败证据：首次清晰模式只失败前台窗口保持断言，后续同样断言复测通过，未证明首次焦点变化根因。设置测试的低完整性构建副本无法准备或写入截图目录；使用 SHA256 完全一致的普通完整性副本后 493 项通过。测试入口补充标准异常报告，避免异常终止掩盖夹具错误。
- 历史 GDI 像素夹具为 505 PASS / 48 FAIL，**没有计为通过，也没有删除或弱化断言**。其 `Pixels()` 用 `GetDC(edit)` + `BitBlt` 读取原生 EDIT 表面；新通道在其上方另有持久 DirectComposition 表面，底层即时 GDI 像素不能代表用户看见的文字。48 项集中于原生表面的松手／方向键字形断言，原日志见 `docs/validation/release-1.2.1.0/hosted.log`。
- 实际桌面复核：正式默认通道不设置原型环境变量，三模式 × 路径栏短文本／重命名长文本共 6 组、833 个采样，均确认合成标记、可见字形，未采到第三种异常文字帧；查看了启动及编辑窗口截图。使用同步方向键消息，不能替代所有物理拖动、输入法、设备重建和跨屏 DPI 验证；用户本人对原型的手动确认单独记录，不包装成自动化结论。
- 发布核验：五个生产 EXE 的文件／产品版本均为 1.2.1.0；便携 ZIP 的 40 项载荷、文件名、哈希、许可证和无 selftest 检查通过。运行依赖检查要求静态 MSVC 运行库，新增必需 MSVC DLL 为 0。便携包 `dist/Pulse-1.2.1.0-portable-win-x64.zip`，SHA256 `FF8E78A7DAF39622CEC2B12E72B74B4C4D227645D92668864339A452370FF608`；安装包 `dist/PulseSetup-1.2.1.0.exe`，SHA256 `107668FB75EB710F1E1D9817E3EED149F0AC7107DDCB1C0CAB4D0885620C81F4`。
- 安装替换：`D:\Applications\Pulse` 的 71 个原文件完整备份并逐项哈希验证；仅替换 5 个变化的 EXE。回退目录 `E:\Projects\pulse\dist\rollback-before-1.2.1.0-20261024-172300`。旧 UI 经原有托盘“退出”正常结束，未强杀；服务恢复原 Running / Auto、注册路径不变，索引 ACL 未改，原网络代理继续运行。
- 原网络代理仍映射的旧 Index 映像暂留安装目录，具体路径见部署结果的 Deferred；没有为删除映像而结束代理。设置和索引目录未作迁移或清理。
- 安装后生产程序实际启动、截图、正常退出 0；随后以普通 Medium 权限重新打开正式 UI。计划中的 7 个载荷哈希和版本匹配。发布采用备份后载荷替换，安装向导本轮没有运行；未声称执行完整历史回归。
- 证据统一位于 `docs/validation/release-1.2.1.0/`：`validation.json`、`artifacts.json`、`deployment-result.json`、`final-installed-verification.json`、测试日志及截图。本节合并到现有唯一报告，不另生成一份完整报告。

**下文为各轮历史记录；当前默认呈现、安装与发布状态以本节为准。**

## 三种文字模式与上游编辑机制的本地实现（2026-10-24）

**本轮已修改本地源码，未打包、发布或替换安装版。** 已接入“自动／清晰／平滑”，恢复上游编辑选区与绘制机制，并统一“清晰”的排版和绘制度量。**编辑闪烁仍未验收通过：最终真实桌面采样在三模式下均捕获到方向键处理中的短暂第三帧，不能宣布闪烁已解决。**

**165 Hz 后续实机对照：** 用户补充原版便携版在宿主机也会闪烁，而虚拟机60 Hz几乎看不出。只读检查确认宿主机当前为 RTX 3070 Ti、3440×1440、165 Hz，非远程会话；有效字体平滑和ClearType均开启，平滑类型2、对比度1200，未取得系统设置损坏的证据。165 Hz帧间隔约6.06 ms，60 Hz约16.67 ms；更高刷新率暴露短暂中间画面与目前证据吻合，但没有在同一宿主机上切换60／165 Hz作因果对照，也没有据此排除驱动或其他合成因素。系统设置没有被修改。

为排除仅为离屏夹具现象，已从上游正式发布资产下载1.0.56便携包，并在隔离配置、隔离文件目录内启动两个真实程序窗口。对相同短文本 Pulse_中文_123.txt、方向键切换与父窗口重绘作独立桌面采样：原版161帧中28帧超出端点像素阈值，本地147帧中5帧超出；两者最坏帧均为文字整行暂时消失，最大差异均620像素。采样调度与次数不同，这些数字不用于比较哪一版更严重，也不等同用户正常操作中的闪烁频率；它们确认当前165 Hz宿主环境下原版也可出现空白帧。脚本只修改隔离路径内的输入文字，以Esc取消，未操作用户文件。

另对本地主窗口永久绑定的透明度effect作单变量A/B：移除绑定后的完整Sharp桌面矩阵仍为89 PASS／4 FAIL，未改善，临时改动已撤回。不能将这一差异列为已确认根因，也不保留无效补丁。原版与本地都存在的短WM_SETREDRAW与layered child呈现时序仍需修复；恢复上游本身不足以保证高刷新率无闪烁。[实机对照脚本](E:/Projects/pulse/bench_data/edit-upstream-ab-20261024/live_compare.py)、[有效显示参数](E:/Projects/pulse/bench_data/edit-upstream-ab-20261024/effective-display-settings.json)、[原版坏帧](E:/Projects/pulse/bench_data/edit-upstream-ab-20261024/upstream-text-address-worst.png)、[本地坏帧](E:/Projects/pulse/bench_data/edit-upstream-ab-20261024/local-text-address-worst.png)。

**更好呈现方式的隔离对照：**本轮未改产品源码、安装版或系统设置。165 Hz宿主、清晰模式、同一短文本路径栏：现有方式131帧／4异常；仅将测试进程的子窗口切为完整位图上传150帧／0异常，但左右端点截图均无文字，已判不可用，不能将全空白算作无闪烁；使用现有原生回退，两轮124及137帧／均0异常，文字可见。它们是同步方向键与父窗口重绘压力采样，不能当作正常操作闪烁率，也没有覆盖真实鼠标、长路径、重命名或输入法。原生绘制可作为稳定性优先的备选，但会改变自绘选区及三模式文字效果的一致性。建议优先验证：原生EDIT继续负责输入、撤销和IME，自绘画面作为主窗口DirectComposition树中的持久编辑层，上一幅完整画面保留到下一幅准备好，再替换；避免更新时隐藏可见表面，也有机会省去每次GPU读回和GDI搬运。这只是待验证架构方案，尚未实施，需证明输入焦点、候选框、裁切、DPI、设备重建及原生回退正确。[隔离对照数据](E:/Projects/pulse/bench_data/edit-upstream-ab-20261024/presentation-investigation.json)。

**持久合成原型验证（2026-10-24）：**已实现并构建仅由测试进程环境变量 PULSE_TEST_COMPOSED_EDIT=1 启用的原型；默认通道、安装版及 E:/Projects/TestPrograms/Pulse 均未替换。EDIT保持可见、继续负责输入和IME，主窗口同一设备增加位于子HWND上方的持久编辑合成层，完整GPU画面准备后提交；新通道不使用WM_SETREDRAW开关，也跳过CPU读回与GDI呈现。补齐失焦更新、隐藏/销毁移除、位置尺寸同步和失败回退。最终三模式各153 PASS／0 FAIL，默认通道133 PASS／0 FAIL；覆盖原生Unicode编辑/撤销、拖选提交、100%/150%及部分125%文字路径、模式切换、隐藏重建和强制失败回退。另一次早期Auto仅前台保持断言失败，后续通过，原因未定，旧日志保留。

当前165 Hz桌面上，三模式的暗色路径栏短文本和浅色重命名栏长文本共825帧，另对清晰模式补充浅色路径栏和暗色重命名栏282帧；**最终共1107帧／0超阈值异常**，端点均有文字，并逐次检查合成标记，未以原生回退或全空白误判通过。已查看浅色路径栏及暗色长文本重命名的完整窗口截图，所查看画面位置、裁切和背景正常。这是同步方向键＋父窗口重绘采样，不代表正常操作闪烁率，更不是物理键鼠验收。

**原型仍未完成的验收：** 真实SendInput多次未能打开编辑框（一次受限桌面失焦安全退出，其余焦点检查正确但F4后无EDIT）；扫描码版本相同，关闭原型的默认通道对照也相同。未确定具体原因，未按通过处理。实际中文输入法候选交互、物理鼠标拖选、辅助功能、设备丢失重建和跨屏DPI仍未验收；本轮没有据此切换默认实现或发布安装包。[原型证据](E:/Projects/pulse/bench_data/composed-edit-prototype-20261024/validation.json)、[隔离原型启动脚本](E:/Projects/pulse/bench_data/composed-edit-prototype-20261024/launch-prototype.ps1)。

**模式与设置：** 设置页增加三段选择，保存到 app.json 的 text_render（0／1／2）；缺省或非法值回到自动。选择后更新现有窗口、编辑框、文字格式、布局和测量缓存，无须重启。自动沿用上游可用的 LumaText 普通界面路径；清晰与平滑使用 DirectWrite 的对应参数。编辑框沿用上游 DirectWrite／Direct2D 自绘和原生 EDIT 输入、撤销与 IME，并未因为模式名称而改成另一套输入控件。

**字距：** 清晰使用 GDI_CLASSIC 栅格化及 CreateGdiCompatibleTextLayout(useGdiNatural=FALSE)，普通 DrawText 也使用 GDI_CLASSIC measuring。测宽、排版、高亮、裁切、省略与编辑命中共用 typography 策略；平滑保持自然度量。修正的是绘制与排版模式不匹配，没有针对 co、ed、gram 或特定文件名插入补偿。字体原有的字侧空白仍然存在，不能承诺任意字母之间都无可见间隙。

**编辑机制：** 恢复上游鼠标 anchor／caret 与延迟提交选区，拖动期间不逐帧 EM_SETSEL；复用同一布局进行绘制和命中。保留鼠标释放时的已绘制滚动原点，下一次非鼠标文本或光标变化再跟踪光标，避免松手跳动。移除本地旧的整段拖动冻结与重复视口维护，保留本地快捷键、Esc 静音和中文输入相关业务。颜色、菜单过滤、预览查找等外围编辑入口也统一到同一处理；原生回退路径直接交还 EDIT，保持鼠标和系统光标。

### 本轮实际验证

| 范围 | 实际结果 | 证据及限制 |
|---|---|---|
| 主程序及受影响测试目标 | 构建通过 | 使用 build_release_clean，最后增量构建 pulse、pulse_child_edit_test、pulse_hosted_edit_stability_test 成功；不是安装验收。 |
| 文字策略与缓存 | 29 PASS／0 FAIL | typography.log；真实 Windows DWrite 对照 2 字体、4 字号、6 字符串、3 模式的144组布局，并验证125% DIP路径。Luma路由部分用stub，不视为真实Luma图像验收。 |
| 子编辑框与原生回退 | 最终三模式各133 PASS／0 FAIL | final-child-auto／sharp／smooth.log；含初始呈现失败、失焦后点击、Unicode输入及原生回退。 |
| 路径与对话框编辑集成 | 三模式各11及425 PASS／0 FAIL | address-auto／sharp／smooth.log 和 dialogs-auto／sharp／smooth.log；隔离夹具，不操作用户文件。 |
| 设置点击、持久化和双窗口生效 | 493 PASS／0 FAIL | settings.log；通过实际命中、控制器、效果和窗口消息，配置仅在内存序列化，未改用户实际偏好。 |
| 宿主编辑离屏矩阵 | 后续三模式各553 PASS／0 FAIL | hosted-auto-diagnostic.log、hosted-sharp.log、hosted-smooth-diagnostic.log；不等同桌面无闪烁。初轮Auto与Smooth各有一次552／1，后续未复现；原日志保留，缺少当时坏帧，原因未定。 |
| 真实桌面连续采样 | 最终三模式各89 PASS／4 FAIL | final-desktop-auto／sharp／smooth.log；4个失败均为浅／深主题的路径栏和重命名栏方向键第三帧，拖选采样均通过。未提高像素阈值、删除断言或将失败算作通过。 |

**视觉复核：** 设置页覆盖中英文、620／1100宽、100%／150%、浅深主题与三模式；实际 MainRenderer 列表覆盖中英文、100%／125%／150%、浅深主题与三模式，含搜索高亮和长名省略。已查看窄设置、150%英文暗色设置、中文暗色列表及三模式字距放大对照，未见所查看画面裁切重叠。这些截图不能替代真实输入法操作和用户显示器上的最终体验。中文主字体仍为 Microsoft YaHei UI，英文为 Segoe UI Variable Text，保留本地14 DIP默认字号。

**剩余问题与边界：** 屏幕坏帧可见编辑表面下方窗口内容，且同步记录到短 WM_SETREDRAW 中 WS_VISIBLE 改变。上游源码同样保留这个短 guard，并强制 LWA_ALPHA 重定向子表面；后续正式便携包实机对照也已捕获到空白帧，见前述数据。尝试仅在重绘开关同步调用中拦截 WM_STYLECHANGING，没有阻止可见位变化，候选已移除。没有留下无效补丁，也没有引入全桌面 LockWindowUpdate、额外 overlay 或新的输入／呈现双窗口架构。此项需要后续单独处理；当前恢复上游与三模式接入不代表它已修复。

证据：[最终结果](E:/Projects/pulse/bench_data/sharp-spacing-20261024/final-results.json)、[设置验证](E:/Projects/pulse/bench_data/sharp-spacing-20261024/settings.log)、[三模式字距画面对照](E:/Projects/pulse/bench_data/text-render-settings-20261024/mode-spacing-comparison.png)、[实际列表截图](E:/Projects/pulse/bench_data/text-render-settings-20261024/file-list-zh-CN-100-dark-1.png)。本节继续合并在唯一报告中。

**以下“尚未修改产品”等结论为此前只读调查的历史状态；本轮代码及验证状态以本节为准。**

## 上游“清晰”模式字距与三选项方向追加核验

**用户新方向：保留上游的编辑实现，并提供“自动／清晰／平滑”三个文字选项；先查清“清晰”模式中 co、ed、gram 的细小字距问题。** 本轮完成源码和隔离渲染核验，没有修改产品或替换安装版；三选项尚未接入本地，编辑闪烁也仍未标记解决。

**定位：上游“清晰”把栅格化改成 GDI Classic 并启用网格对齐，但列表文字仍使用 Natural 度量。** `typography.cpp:190-214` 选择 GDI_CLASSIC；`ui_pane_list.cpp:2037-2039` 回退绘制显式使用 DWRITE_MEASURING_MODE_NATURAL，高亮/扩展名则用 CreateTextLayout。微软 DWRITE_RENDERING_MODE 文档将 GDI_CLASSIC 指定给 GDI_CLASSIC 度量；IDWriteFactory::CreateGdiCompatibleTextLayout 文档说明 FALSE 对应 GDI classic 度量、TRUE 对应 GDI natural。

这种组合下，排版保留小数字形 advance，笔画栅格化又贴像素格，可能使相邻字母的可见空隙不均。它没有向字符串插入空格，也不表示显示名称被更改；正常字体自带的 side bearing 和词间空格仍会保留。

**截图复现证据：** 使用本机 Windows DirectWrite + 软件 Direct2D，Microsoft YaHei UI 13 物理像素，重画 appcompat。用户截图与“GDI_CLASSIC + Natural layout”的紧包围二值笔画都是71×12像素，平移搜索±2像素后差异为0，最佳平移为0。判断阈值为任一RGB通道超过区域众数背景75，排除红色标记；该结果不是逐RGB像素相等，也不是三个标框全部逐像素验证。Downloaded Program Files 已做同配置截图对照。

另外渲染2种字体×3个物理字号（13／16.25／19.5）×4种排版/绘制组合×3个字符串，共72组布局，所有Windows绘制调用成功。GDI-compatible布局改变了字距分布：13px雅黑UI下，appcompat总advance从71.0874变为70，Downloaded Program Files从168.099变为164。该实验说明度量差异真实存在；不据此宣称每对字母间隙均已消除，或完成真实主程序验收。

**后续实现约束：** 保留自动、清晰、平滑的选择及持久化，沿用上游的完整编辑选区与表面逻辑。清晰模式的测宽、排版、裁切、省略、着色分段、光标命中与缓存失效须使用同一度量策略；可比较 GDI-compatible排版配GDI_CLASSIC，或Natural排版配NATURAL+gridfit。不要只改单个DrawText调用，也不要把列表和编辑框永久强制为同一种渲染后端。不同策略的清晰度和字距需要实际主程序画面对照后再定稿。

主程序未构建、安装版未改。证据：[对照源码](E:/Projects/pulse/bench_data/sharp-spacing-20261024/probe.cpp)、[appcompat放大对比](E:/Projects/pulse/bench_data/sharp-spacing-20261024/comparison-appcompat.png)、[Downloaded Program Files放大对比](E:/Projects/pulse/bench_data/sharp-spacing-20261024/comparison-downloaded.png)、[逐字符位置](E:/Projects/pulse/bench_data/sharp-spacing-20261024/positions.csv)、[截图二值核验](E:/Projects/pulse/bench_data/sharp-spacing-20261024/screenshot-match.json)。本节仍合并在唯一报告中。

## 上游 1.0.56 编辑框实现对照（2026-10-24）

**结论：上游 1.0.56 与本地 1.2.0.3 的编辑框绘制路径存在实质差异，之前改成原生 EDIT 的做法没有沿用上游当前完整方案。用户已实测最新版无该现象；本轮完成固定版本源码审查，未修改产品、构建或替换安装版，当前轻微闪烁仍未解决。**

截至本轮查询，GitHub `releases/latest` 指向 **v1.0.56（2026-10-16）**；tag 解引用及 main 都指向 `c4fe01ed82fe091f495f531972a854aa32caac2b`。已下载该提交的源码 ZIP 并核对 `version.txt=1.0.56`，没有把旧 baseline 当最新版。ZIP SHA256：`A32621CF9A74754E1561C46EB6BFA013607114ED63FC8097048CF26465E75F1A`。匿名 REST 查询限流，版本通过发行页和 Git refs 交叉核实。

| 项目 | 上游 1.0.56 的源码事实 | 本地当前状态与含义 |
|---|---|---|
| 绘制所有权 | `app_hosted_edit.cpp:114-127,600-628`：创建重定向 child EDIT，四种宿主过程统一进入 Handle/DefPresented；EDIT 保留文本、撤销、IME，自绘层处理显示 | `src/app/app_hosted_edit.cpp:95-105,511-652`：路径、搜索、筛选、文件/标签重命名走原生 EDIT；1.2.0.3 已绕过自绘，不能把其他对话框仍使用的 edit_host 自绘直接视为当前路径栏根因 |
| 实际文字后端 | `lumatext_renderer.cpp:1195-1359`：入口虽名为 LumaText，编辑文字实际通过 DirectWrite layout + Direct2D DrawTextLayout，采用灰度抗锯齿；命中与选区使用同一布局 | 当前五类宿主使用 GDI EDIT 原生绘制；其他自绘控件还走旧 LumaText 字形与命中，不能只恢复旧 wrapper 认为等同于上游 |
| 拖动选区 | `lumatext_renderer.cpp:1050-1167,1500-1535`：移动时更新内部 anchor/caret 和 deferred 标志；松开、失去捕获以及复制/编辑等必要操作前提交 EM_SETSEL，避免每次移动都经过原生 redraw guard | 本地旧自绘 `lumatext_renderer.cpp:873-945,1099-1138` 仍每次移动 EM_SETSEL，并用捕获期间 WM_SETREDRAW guard。当前路径/重命名已走原生，属于后续迁移必须成套替换的旧机制，不能称为已证明的当前根因 |
| 方向键与视口 | `lumatext_renderer.cpp:1016-1036,1089-1093,1392-1470`：保存绘制偏移，命中使用上次已画出的偏移；同步文本/键盘/IME后的选区，记录选区活动端及销毁清理 | 本地旧自绘通过捕获期间冻结偏移、越界单独滚动处理，缺少上游完整同步机制；直接复用存在反向扩选、释放后跳动风险 |
| 合成表面 | `edit_host.h:39-58`，`ui_compositor.cpp:221-238`：LWA_ALPHA 交给 Windows 重定向；自绘位图送入该窗口 DC，避免将这一模式与 UpdateLayeredWindow 混用 | 本地当前宿主也设置 LWA_ALPHA，compositor 中也已有 redirected DC 分支；表面策略并非完全缺失。不能把 WS_EX_LAYERED 本身、或不加 WS_CLIPCHILDREN，直接认定为差异根因 |
| 字体 | `typography.cpp:147-153,277-289`：简体中文 Microsoft YaHei UI；英文 Segoe UI Variable Text；集中创建编辑字体，DirectWrite 灰度绘制 | 本地简体中文同为 Microsoft YaHei UI、默认14 DIP；字体家族相同，观感差异主要候选是 GDI/DirectWrite 及渲染参数，但尚未实测归因 |
| 重复布局 | `app_hosted_edit.cpp:106-111` 同样先判断 margins 是否改变，仍每父帧 SetWindowPos；主窗口同样使用 WS_EX_NOREDIRECTIONBITMAP | 这些行为两边共有，本轮没有证据据此宣布闪烁根因；上轮重复布局计数不等同于可见闪烁证据 |

上游源码明确将拖动闪烁与每次移动触发 EM_SETSEL / WM_SETREDRAW 造成重定向表面短暂隐藏关联；这是上游代码注释和处理机制的事实，不能代替对本地当前原生绘制版本的根因验证。上游也保留键盘等变化时短暂 redraw guard，所以不能将其概括为彻底不使用 WM_SETREDRAW。

**建议修复路线：** 以该固定提交的完整编辑显示流程作为基线，小范围迁移 DirectWrite 布局/绘制、延迟选区提交、选区活动端同步、共享视口及重定向表面处理；删除这一范围内已被替代的捕获冻结、原生直绘旁路，保留本地业务快捷键、Esc静音及其他已确认行为。不要只复制某一个 flag，也不合并上游无关新功能。

收益：编辑文本、命中、选区使用一致度量，采用用户已经实测正常的上游方向。成本和风险：并非一行补丁，需核对 compositor/renderer/typography 接口与退回原生路径，尤其是反向 Shift 扩选、复制剪切、中文 IME、越界滚动和 DPI。

验收应比较同机上游与修复版，在中文、当前缩放/主题下使用真实路径栏和真实文件重命名，覆盖长短文本、方向键/Shift、快速双向拖动、越界后释放、双击选词、输入法替换；观察连续实际桌面帧，确认没有整行空白、第三种文字帧或释放后原点跳动。历史553项和192帧结果不充当本次验收。本轮只读，不运行这些功能验收，也不宣称已修好。

证据：[固定源码与比较元数据](E:/Projects/pulse/docs/validation/upstream-edit-review-20261024/outcome.json)；固定源码快照位于 `bench_data/upstream-edit-review-20261024/pulse-c4fe01ed82fe091f495f531972a854aa32caac2b`。本节合并在唯一报告中。

## 1.2.0.3 剩余轻微闪烁与实际字体核查

**当前问题状态：用户确认 1.2.0.3 仍有整行文字或选区一起轻微闪烁；根因尚未确认，本轮不标记修复完成。安装版保持 1.2.0.3，本轮仅增加诊断测试和证据，没有更改产品字体或替换安装文件。**
- 字体核实：中文界面使用 Microsoft YaHei UI（微软雅黑 UI），普通字重 400，字符高度 14 DIP 随缩放换算；英文界面指定 Segoe UI Variable Text。生产宿主中文夹具在 100%/150%/200% 下的 GetTextFaceW 均确认实际选中 Microsoft YaHei UI；另用相同 CreateFontW 参数查询系统，三个候选字体均未发生字体替换。代码位于 src/app/app_hosted_edit.cpp:124 和 src/ui/typography.cpp:126；[系统映射证据](validation/font-flicker-20261024/native-font-resolution.json)。
- 1.2.0.3 编辑文字改为 GDI/原生 EDIT，字体质量参数为 ANTIALIASED_QUALITY；这一绘制方式变化可能解释与正文文字的观感差异，尚不能以此解释用户所见整行闪烁。字体核查结论与闪烁根因分开记录。
- 新增诊断发现，每 10 个父窗口渲染帧，路径/搜索/筛选仍有 20 次 WM_WINDOWPOSCHANGING，重命名/标签有 10 次；同一观察区间 WM_PAINT 和 WM_ERASEBKGND 均为 0。确认存在冗余布局调用，但尚无证据证明它造成所报闪烁，未以此作推测性产品修复。
- 中文定向路径检查 553 PASS / 0 FAIL；之后补齐测试父窗口与生产一致的擦除、绘制及通知处理，移除夹具独有的 WS_CLIPCHILDREN，加入独立线程从桌面采样。中文、100%、深浅主题、路径与重命名、方向键与框内拖选共采集 192 个桌面帧，未观察到预期两个选区/光标画面之外的第三帧；该有限范围检查为 93 PASS / 0 FAIL，不代替用户实际使用条件。
- 拖选诊断首轮为 92 PASS / 1 FAIL，抓到第三个选区画面；隔离脚本拖选期间 Windows 补发的真实指针移动后，93 PASS / 0 FAIL。原失败日志和截图保留，不将这一测试输入隔离调整算作产品 bug 修复。
- [中文字体与路径日志](../bench_data/edit-surface-20261024/font-trace-zh.log)、[独立桌面采样](../bench_data/edit-surface-20261024/selection-isolated.log)、[原失败日志](../bench_data/edit-surface-20261024/selection-before.log)、[原失败截图](validation/font-flicker-20261024/selection-before-worst.bmp)。尚未稳定捕获用户所见闪烁；没有据自动检查通过宣称现场问题已消除。

**1.2.0.3 已发布和已通过上述自动化检查，不代表用户反馈的轻微闪烁已经解决。当前问题状态以本节为准。**

## 1.2.0.3 编辑框原生绘制与实际画面复核

**当前状态：1.2.0.3 已完成本轮修复、定向验证、便携包和安装包；安装版已替换并以普通用户权限重新打开。** 1.2.0.2 的内部视口测试没有覆盖真实输入期间的中间画面，不能证明用户报告的闪烁已消除。

- 确认的生产路径问题：原生 EDIT 在处理方向键时已经更新 GDI 画面，外层 LumaText 随后又绘制另一套字形/选区/光标；拖选期间冻结自绘滚动、松手后恢复跟随，会改变文字原点。鼠标选区活动端缓存也不能完整代表键盘反向扩选。路径、地址搜索、筛选、文件重命名、标签重命名共享该宿主路径。
- 修复：以上五种宿主编辑框统一由 Windows EDIT 负责字体、绘制、光标、选区、命中、水平滚动和输入法消息；继续使用不透明的重定向子窗口表面，保证文字出现在 DirectComposition 主窗口上。删除此处的重复自绘、手动擦除和自绘颜色/字体转发辅助函数；保留实际业务快捷键、回车/Esc 行为。补齐 DPI/语言切换时筛选框的原生字体更新。全局搜索和菜单/对话框的独立编辑实现没有在本轮切换。

| 定向验证 | 结果 | 实际覆盖 |
|---|---:|---|
| 五种实际生产宿主编辑框 | 553 PASS / 0 FAIL | 深浅主题，100%/150%/200%，连续父窗口渲染，方向键中间画面、短文本字形稳定、长文本拖选/松手、真实活动光标、中文插入和撤销、保留标签快捷键。[日志](E:/Projects/pulse/bench_data/edit-surface-20261024/corrected.log) |
| 真实桌面合成复核 | 85 PASS / 0 FAIL | 独立临时窗口中的路径栏和重命名栏，深浅主题、100%；从桌面取像素，原生表面与桌面一致，父窗口合成后可见，方向键只改变光标像素。包含重复基础断言，不叠加声称 85 个新场景。[日志](E:/Projects/pulse/bench_data/edit-surface-20261024/desktop.log) |
| 实时搜索编辑 | 11 PASS / 0 FAIL | 连续查询更新、连续输入、文本中部插入、鼠标落点后的输入顺序。[日志](E:/Projects/pulse/bench_data/edit-surface-20261024/address-final.log) |

- 像素证据：五类 × 两主题 × 三缩放共 30 组，方向键的原生中间表面与最终表面差异均为 0 像素；拖选松手差异也均为 0 像素。短文本方向键最大变化分别为 38/56/74 像素，恰好是两个位置的一像素宽光标，未发现字形位置变化。
- 测试诊断保留：原始修复前日志为 375 PASS / 58 FAIL；最初修复后为 521 PASS / 32 FAIL，未据此交付。逐帧位图证明其中 28 项来自截图抗锯齿颜色通道仅差 1/255；另 4 项集中在深浅主题的 200% 搜索框（各含短文本和拖选断言），测试仍固定为 1100 物理像素、输入区仅宽 48 像素，出现正常溢出滚动及越界拖选返回空选区，不适用“短文本字形不动”和最终选区必非空的前提。最终测试固定 1100 DIP，只忽略一色阶量化差异，保留光标/字形位移检测。没有删除失败日志或跳过断言；修改了测试条件，因此不将前后通过数视为完全相同测试集的对比。
- 历史失败和逐帧证据：[修复前](E:/Projects/pulse/bench_data/edit-surface-20261024/before.log)、[首轮修复后](E:/Projects/pulse/bench_data/edit-surface-20261024/after.log)、[量化与窄框诊断](E:/Projects/pulse/bench_data/edit-surface-20261024/diagnostic.log)。没有运行全部历史回归或完整 selftest；旧 rename selftest 同步改为检查原生光标和实际选区表面，本轮未独立运行该旧入口。中文字符插入验证不等于各输入法候选窗口均已验收。

- [1.2.0.3 便携包](E:/Projects/pulse/dist/Pulse-1.2.0.3-portable-win-x64.zip)：SHA256 `0F895CE698E91DE0DD87806BF94A68B72A960800736BD3D4D94B3762146F2635`。
- [1.2.0.3 安装包](E:/Projects/pulse/dist/PulseSetup-1.2.0.3.exe)：SHA256 `BE09607D4F8027D5C0BAF720C1A23F165862CF7ABF7642287AFE2C11D2B0A7E6`。
- 发布核验：40 项便携载荷逐项匹配，五个生产 EXE 均为 1.2.0.3，原始文件名、运行库和许可证检查通过。安装包已编译并记录文件身份，本轮通过保留状态的载荷替换更新现有安装，没有运行安装向导。[发布证据](E:/Projects/pulse/docs/validation/edit-surface-20261024/artifacts.json)。
- 安装目录 `D:\Applications\Pulse`：只替换 5 个发生变化的 EXE；完整 70 文件备份逐项验证，回退目录 `E:\Projects\pulse\dist\edit-surface-1.2.0.3-rollback-20261024-140547`。旧界面经正常托盘退出；没有强杀。设置、索引数据和索引目录权限保留，服务恢复原 Running / Auto，原网络代理继续运行。
- 原网络代理仍映射的上一版 Index 映像暂留安装目录，未为清理该映像而结束代理；本轮不改索引策略。[完整部署与保留项记录](E:/Projects/pulse/docs/validation/edit-surface-20261024/deployment-result.json)。
- 安装后生产程序实际启动、截图、正常退出 0，随后重新打开普通 Medium 界面；7 项计划载荷哈希匹配。[安装后核验](E:/Projects/pulse/docs/validation/edit-surface-20261024/final-installed-verification.json)、[生产启动证据](E:/Projects/pulse/docs/validation/edit-surface-20261024/installed-smoke-result.json)。实际桌面定向检查使用同源生产宿主函数的隔离夹具，没有宣称已经完成用户本人手动验收或所有动画检查。

**用户已确认 1.2.0.2 仍有闪烁和晃动，旧轮测试不足以验收该问题。下文旧版“修正”仅为历史记录，当前结论以本节为准。**

## 1.2.0.2 快速拖选文字左右晃动修正

**当前状态：1.2.0.2 已完成修复、定向验证和两个发布包。安装文件、实际启动、普通用户界面和原服务状态均已核验。**

- 确认原因：长文本已经水平滚动时，鼠标命中会舍入到视口边缘之外；原先每次命中和重绘都跟随选区活动端调整水平偏移，造成下一次命中使用不同文字原点，快速往返拖选出现左右晃动。路径栏和重命名栏共用该编辑呈现实现。
- 改动：捕获鼠标拖选期间保持框内视口固定；仅指针越出客户区时由输入事件调整滚动，重绘本身不推进滚动。文本修改和松开鼠标后的光标跟随继续工作。此前原生编辑与自绘表面交替造成的闪烁修复继续保留。
- 复现与回归：已滚动长文本在 100% / 150% 缩放、两种窗口呈现模式中，修正前出现 4 项文字原点稳定性失败；修正后正式 pulse_child_edit_test 为 **113 PASS / 0 FAIL**。覆盖快速往返拖选、原生选区更新、左右越界滚动、无新输入的重复重绘、松开后光标移动和短文本重置。
- [原失败日志](../bench_data/quality-release-1.2.0.1/drag-viewport-scrolled-before.log)、[修正后日志](../bench_data/drag-viewport-1.2.0.2/hosted-edit.log)、[退出记录](validation/drag-viewport-1.2.0.2/hosted-edit-exit.json)。这些是直接调用生产编辑控件与绘制路径的自动测试，未宣称完成真实鼠标手动验收或所有动画检查。
- 从已有完整 Ninja Release 目录仅构建受影响编辑目标；版本资源统一后构建五个生产交付目标。未运行所有历史回归或完整自测。构建依赖与本地化编译修正沿用上一轮，未复用有空依赖的旧 NMake 对象。

- [1.2.0.2 便携包](../dist/Pulse-1.2.0.2-portable-win-x64.zip)：12,405,203 B；SHA256 `BE451CCF29708C9681F43731D0FA5C87D55435C6B264924B479D7ACAB65F6C61`。
- [1.2.0.2 安装包](../dist/PulseSetup-1.2.0.2.exe)：10,076,101 B；SHA256 `BDD609D0BC8F66496A6F8EEAC4D244BED2A1143F3E8E121B64C908D5506D3FFA`。
- [发布核验](validation/drag-viewport-1.2.0.2/artifacts.json)：40 项便携载荷逐项匹配，五个 EXE 版本与原始文件名一致，无测试和调试文件混入，运行库与许可证检查通过。安装包已编译并记录文件身份，未运行安装向导或单独解包核对。
- [安装前生产启动](validation/drag-viewport-1.2.0.2/preinstall-smoke-result.json)：使用与构建文件哈希一致的普通 Medium 副本，实际启动、截图并正常退出 0；截图已查看。截图检查只证明启动和静态布局，不代替拖选动态测试。
- 安装目录 `D:\Applications\Pulse` 已更新为 **1.2.0.2**，只替换 5 项变化文件。完整 70 文件回退备份逐项哈希验证，保存在 `E:\Projects\pulse\dist\drag-viewport-1.2.0.2-rollback-20261024-121648`。
- 正常托盘退出旧 UI，未强杀用户程序；保留设置、索引数据和索引目录权限，PulseIndex 恢复原来的 Running / Auto 状态。所有计划载荷与安装文件哈希相同，随后以普通用户重新打开界面；[部署记录](validation/drag-viewport-1.2.0.2/deployment-result.json)、[安装后核验](validation/drag-viewport-1.2.0.2/final-installed-verification.json)、[安装后启动](validation/drag-viewport-1.2.0.2/installed-smoke-result.json)。
- 安装目录中仍由原网络代理映射的历史旧映像保持原样；没有为清理备份而终止代理。此轮没有更改索引或搜索算法，也未重复运行索引性能测试。

**下文 1.2.0.1 及更早版本保留当轮结果；当前安装与发布状态以上节为准。**

## 1.2.0.1 质量改进、打包与安装更新

**当前状态：已完成本轮代码质量改进、定向验证和两个 1.2.0.1 发布包。安装版文件、真实启动、用户界面和服务状态均已核验。完成日期：2026-10-24（Asia/Shanghai）。**

### 9 项审查问题的处理

| 原项 | 改进结果 |
|---|---|
| 迟到 runas 破坏稳定菜单 | 仅初始快照可提升 COM runas；本次菜单中首次晚到项留动态区，下次打开再提升。冷菜单→部分/最终异步更新回归先复现 3 FAIL，再通过。 |
| USB 目录挂载路由 | 后台枚举卷的全部挂载别名；已知 GUID 复用服务 kind/online，未知卷仅探测一次，UI 只读规范化缓存并按最长目录边界匹配。IPC 字节格式不变。 |
| 常规测试接口滞后 | 补充主机测试直接协议依赖；旧离线测试按当前 Flatten/MFT builder 改写，保留 FRN、隐藏/排除、搜索、持久化、重启和恢复断言；未添加产品空实现。 |
| 测试依赖临时 rsp | 编辑框、地址栏、USB 路由、尺寸缓存与设备策略都有正式 CMake 目标。四个应用回归共用一套生产对象；不再需要旧 bench_data 私有链接输入，移除路由测试中固定本机 DLL 路径。 |
| 每帧全目录业务工作 | 请求按 snapshot/范围/详情路径失效；排序按面板生命周期、snapshot、方向、分组模式、尺寸修订失效。稳定帧不构造全量请求、不重复 Sync，也不重复排队/排序。保留 worker 容量恢复重试。 |
| 导航平行栈叠加 | 六个平行容器收为前后两个 HistoryEntry 栈，路径、搜索来源、双轴视口在同一记录保存；删去长度不符时整栈丢元数据的补偿。旧路径记录仍可缺省字段。 |
| 无效慢扩展策略 | 删除 deferred、慢成功计数及其触发的 UI 设置写盘；保留明确禁用和旧 JSON 自动禁用迁移，未恢复自动禁用。 |
| 性能工具误导 | 明确样本数；单次显示 single，输出全部原始样本、min/median/max 与 nearest-rank p95；质量与时间预算分开，退出码仍由 enforce 合并验收。增加统计边界检查。 |
| 旧优化残留 | 删除 index_changed 假指标及无效断言；旧分卷查询声明、字段、实现和调用仅在 storage 对照目标的宏内编译。生产只保留 aggregate；旧格式、WAL、回退和其他必要授权回调不删。 |

### 正式目标实际验证

| 检查 | 结果 | 日志 |
|---|---:|---|
| 正式编辑框 / 主题缩放 | 313 PASS / 0 FAIL | [日志](../bench_data/quality-release-1.2.0.1/hosted-edit.log) |
| 正式地址栏持续输入 | 11 PASS / 0 FAIL | [日志](../bench_data/quality-release-1.2.0.1/address-edit.log) |
| 正式 USB IPC 搜索路由 | 11 PASS / 0 FAIL | [日志](../bench_data/quality-release-1.2.0.1/removable-routing.log) |
| 正式目录尺寸缓存 / 排序 | 15 PASS / 0 FAIL | [日志](../bench_data/quality-release-1.2.0.1/folder-sizes-ui.log) |
| 正式设备策略 / 挂载枚举 | 43 PASS / 0 FAIL | [日志](../bench_data/quality-release-1.2.0.1/removable-policy.log) |
| 生产模式候选缓存 / 名称池 | 62 PASS / 0 FAIL | [日志](../bench_data/quality-release-1.2.0.1/index-cache.log) |
| 旧分卷与 aggregate 对照 | 42 PASS / 0 FAIL | [日志](../bench_data/quality-release-1.2.0.1/index-storage.log) |
| 压缩 / 损坏拒绝 / A/B 恢复 | 45 PASS / 0 FAIL | [日志](../bench_data/quality-release-1.2.0.1/index-compaction.log) |

本轮子任务先前已完成：导航模型 69/0；冷菜单 runas 10/0；7-Zip/显式禁用兼容 10/0；离线保留 15/0；旧格式修复 44/0；主机目录尺寸协议 5/0；扫描 23/0、2 SKIP。前三项只保留在子任务工具输出，没有落盘日志，不伪造日志链接或为重复计数重跑；其余日志分别在 bench_data/quality-offline-retention.log、quality-legacy-repair.log、quality-host-folder-sizes-medium.log、folder-sizes-quality/scan-results.log。各测试范围有重叠，不相加宣称覆盖率。

### 过程中发现的失败与修正

- 用户看到的 pulse_folder_sizes_ui_cache_test.exe 弹窗是本轮新增排序缓存实现的真实空地址写入：vector::insert(end(), {}) 选中空 initializer_list 重载，返回 end 后被解引用。已改为 emplace_back 后取末项，保留原断言；此失败发生在安装替换前。
- 随后双面板共享旧 snapshot 的转移优化造成 3 项计数断言失败。删除跨面板转移，统一在新 snapshot 身份改变后各重建一次；新增明确的一次失效断言，保留跨 750 ms 的 100 稳定帧强断言，最终 15/0。SetErrorMode 只防止测试崩溃反复弹框，不改变异常退出码或绕过检查。
- 首次长构建中断；恢复后的受限环境链接器停在等待并留下部分 EXE。核对命令行后仅结束本轮构建树，在普通 Medium 环境重新完成。未强制结束用户 UI 或安装服务。
- 新路由测试目标首次因没有继承 /MANIFEST:NO 发生重复 manifest 链接失败；正式质量目标统一继承生产链接选项，已重新构建和验证。
- 首次实际安装的隔离截图启动发生 0xC0000005，立即从69文件校验备份回退旧版并重新打开，保留失败候选快照。生产对象166份依赖文件中99份为空；39个直接包含模型头的源中35个对象早于头文件且依赖为空，形成新旧模型布局混用。实测编译器只有2052中文资源，VSLANG=1033仍回退中文，不能假定实际输出英文。现在用UTF-8编译包装器与全新Ninja目录build_release_clean，拒绝复用本地化NMake依赖；发布脚本默认build-release并可指定目录。新app_navigation依赖记录302项，明确包含app_state.h/app_model.h。未用带测试宏的quality对象替换生产。
- 所有早期日志保留；不把中断、环境失败、重复资源或首次崩溃说成通过。最终生产程序只有五个交付目标，PULSE_WITH_SELFTEST=OFF；没有默认运行完整旧自测或全部历史测试。
- 全新构建EXE继承Low完整性标签，而截图目录为Medium，直接运行的截图写入返回1；未把它记为产品通过。将七项载荷按字节写入Medium隔离目录、逐项核对哈希后，生产启动与截图退出0，再进行最终部署。安装目录中的最终启动也退出0。未改用户全局ACL或用提权运行掩盖产品行为。

### 发布包

- [便携包](../dist/Pulse-1.2.0.1-portable-win-x64.zip)：12,405,127 B；SHA256 `A121ECF14E2FFA7DEA7A68A27E76C200CB40064AC33A0E24E87888F5A4E16A3D`。
- [安装包](../dist/PulseSetup-1.2.0.1.exe)：10,073,545 B；SHA256 `5632538DEE84F40F5FDE84C2706DFF37F5CE8C4379CA323B259B2B94D75F7EBD`。
- [逐项核验](validation/quality-release-1.2.0.1/artifacts.json)：40 项便携载荷与 build/仓库资源大小和 SHA256 全匹配，五个 EXE FileVersion/ProductVersion/OriginalFilename 正确，无测试或调试文件混入，无内置自测，路径与许可证检查通过。安装包编译成功并记录身份，未单独解包比对或运行安装向导。

### 安装与验证边界

- 完整备份并核验 69 个原安装文件，恢复目录 `E:\Projects\pulse\dist\quality-release-1.2.0.1-final-rollback-20261024-115519`；实际替换 `Pulse.exe, Pulse.Index.exe, Pulse.Shell.exe, Pulse.Preview.exe, Pulse.Document.exe`。其余原文件按备份哈希逐项核对。
- 安装版采用正常托盘退出、管理员原位替换和恢复原 PulseIndex 启动/运行状态；没有运行会卸载旧版的安装向导，不改索引格式、索引位置或用户配置，不重建索引。
- [部署结果](validation/quality-release-1.2.0.1/deployment-result.json)，保留仍被旧进程占用的已知哈希映像：1 项。
- 安装路径隔离截图启动：退出码 0；结果见 [记录](validation/quality-release-1.2.0.1/installed-smoke-result.json)。静态截图不能替代物理拖选及动画连续帧验收。
- 普通用户界面及网络代理恢复状态见 [记录](validation/quality-release-1.2.0.1/user-ui-restored.json)。
- 原网络代理保持原进程，不强制终止；它占用的一份旧Index映像按已知哈希保留。磁盘上五个正式程序均为1.2.0.1，服务已重启加载新程序；旧代理下一次正常退出后该退休映像才可回收。
- [最终安装核验](validation/quality-release-1.2.0.1/final-installed-verification.json)记录五EXE版本、七项载荷SHA256、Running/Auto服务及持续运行主界面。
- USB 实际目录挂载/热插拔、真实 UAC 菜单命令与第三方菜单 Invoke、路径栏物理拖选未作为本轮运行验收。设备验证包括真实系统卷枚举与模拟别名经过生产 IPC 处理和路由。
- 本轮改的是代码质量及验收口径，没有承诺把宽泛百万匹配查询压到 150 ms。安装后性能如仍超预算应按实际日志保留失败，不能以协议成功或单词毫秒级响应掩盖。

实际安装服务6词×5次共30次观测，全部完整且无协议错误：True；质量与时间预算分开输出。enforce退出码1，宽泛a/ab的p95预算未通过，不能称速度全部达标。
[实际搜索原始样本](../bench_data/quality-release-1.2.0.1/installed-search.log)。本轮a中位291.68ms、ab中位26.22ms，abc/report/pdf中位约6.05/2.08/2.05ms；五样本分位不代表长期p95。

**以下代码质量审查保留修复前状态；当前改动、验证与安装状态以本节为准。**

## 最后代码质量审查（只读）

**结论：存在值得整理的技术债，并发现两条有明确源码依据的功能风险；不能把当前代码称为已经完全干净。** 主要问题是接口变更后的测试收尾、无效策略残留、绘制路径承担重复业务工作，以及不同模块对同一身份或菜单稳定性的契约不一致。未发现必须全面重写索引或 Shell 隔离架构的依据。

本轮以当前 79 个已跟踪文件的 diff、相关新增文件、直接调用链和已有失败记录为入口。只读审查产品代码，没有修复产品、清理文件、修改用户设置或替换安装程序。本文不是全仓每一分支的无缺陷保证。优先级 P2 为应安排处理，P3 为低优先级清理；结构风险与已复现运行故障分开表述。

### 1. P2：迟到的 runas 提升与稳定菜单行约束冲突

位置：[context_menu_controller.cpp:289](../src/app/context_menu_controller.cpp#L289)、[fluent_menu.cpp:134](../src/ui/fluent_menu.cpp#L134)、[app_commands.cpp:1074](../src/app/app_commands.cpp#L1074)。

BuildDisplay 可以把晚到的 COM runas 插入 Open 后，而 ReplaceLiveItems 要求原有稳定行的索引不变。例如初始 `[Open, Properties, More(live)]` 变为 `[Open, RunAs(live), Properties, More(live)]` 时，Properties 从索引 1 移到 2，替换函数在稳定行核对时拒绝，调用者没有补救发布路径；同批其他 Shell 行也无法刷新。现有管理员菜单测试预装静态 runas，未覆盖首次晚到场景。

这是局部菜单提升补丁没有同步现有异步契约的例子。最小建议：本次菜单中首次晚到的 runas 保留在动态区，下次打开再提升；或事先预留稳定槽位。不要直接放宽全部稳定行校验。验收需覆盖冷菜单先显示、runas 及其他 Shell 项随后到达、悬停项不跳动和整批更新成功。**证据为源码路径推演，本轮未运行该场景。**

### 2. P2：USB 排除策略与临时搜索路由没有共享完整的卷身份

位置：[persistent_index_policy.h:128](../src/index/persistent_index_policy.h#L128)、[index_config.cpp:343](../src/index/index_config.cpp#L343)。

后台 ProbePersistentDevice 按实际卷解析路径，但 UI 临时搜索判断只按缓存 VolumeInfo.mount_point 做字符串前缀匹配，而缓存来源只枚举盘符。若 USB 卷挂在 `C:\mounted-usb`，用户从该目录访问，后台会正确禁止持久索引，但 UI 看到的 C 盘仍是内部盘，无法识别这个挂载别名，文件名及内容搜索不能按预期切到临时扫描。现有 mounted fixture 没有验证真实卷别名经 IPC 发布后的路由。

最小建议：后台发布卷的全部挂载路径，前端统一用最长路径匹配；继续避免 UI 线程实时设备探测。验收包含同一 USB 卷的盘符、目录挂载别名、较深子目录、移除与缓存失效。**源码链路确认，未使用本机设备实测。**

### 3. P2：产品接口删掉后，常规回归入口没有同步维护

位置：[index_engine_test.cpp:39](../src/bench/index_engine_test.cpp#L39)、[index_engine_outstanding_cases.h:84](../src/bench/index_engine_outstanding_cases.h#L84)、[index_engine_test.cpp:601](../src/bench/index_engine_test.cpp#L601)、[index_host_stress.cpp:1](../src/bench/index_host_stress.cpp#L1)、[CMakeLists.txt:222](../CMakeLists.txt#L222)。

常规 pulse_index_engine_test 仍调用已不存在的 PreserveOfflineVolumesLocked；pulse_index_host_stress 缺少它直接使用的 transport 与 folder-size 协议头。它们仍是 CMake 常规可构建目标，现有日志已经记录编译失败。私有驱动补头或抽取单一 fixture 可以支持局部验证，不能替代正式入口可构建。

最小建议：按当前行为改写旧离线卷测试，补齐直接依赖；不要删测试或用空实现恢复编译。只构建并运行相关目标，不因此重跑全部历史回归。**本轮检查当前源码并复核既有失败日志，没有重新构建。** 证据：[引擎失败日志](../bench_data/finish-all-20261023/build-intern-all-paths.log)、[主机失败日志](../bench_data/filename-index-only-20261023/host-stress-build.log)。

### 4. P2：新增回归依赖被忽略的临时构建环境，不能随源码稳定复现

位置：[hosted_edit_stability_test.cpp](../src/bench/hosted_edit_stability_test.cpp)、[.gitignore:73](../.gitignore#L73)、[CMakeLists.txt](../CMakeLists.txt)、[临时构建器](../bench_data/hosted-edit-flicker-20261023/build_probe.py)。

hosted_edit_stability_test.cpp、address_edit_stability_test.cpp 等没有对应 CMake 注册。前者的实际构建脚本从 build/CMakeFiles/pulse.dir/objects1.rsp 和另一轮 bench_data/issue-confirm-external/libraries.rsp 拼接链接输入，脚本本身也被 /bench_data/* 忽略。当前机器能够运行，不代表干净检出能构建；测试与修复的长期绑定不足。

最小建议：把有长期价值的回归接入现有架构 fixture 构建开关或可版本化的定向驱动；保留隔离窗口与断言，临时历史证据继续留在 bench_data，不把全部诊断产物纳入产品。验收以缺少旧 bench_data 的工作目录能配置、构建和运行该目标为准。**这是本轮验证工作流留下的债务，不能只归给原项目。**

### 5. P2：按大小排序把全目录请求维护绑在每次绘制上

位置：[app_main.cpp:203](../src/app/app_main.cpp#L203)、[folder_sizes_ui.cpp:43](../src/app/folder_sizes_ui.cpp#L43)、[folder_sizes_ui.cpp:71](../src/app/folder_sizes_ui.cpp#L71)、[folder_sizes_ui.cpp:136](../src/app/folder_sizes_ui.cpp#L136)、[folder_sizes.cpp:375](../src/app/folder_sizes.cpp#L375)。

大小排序时，每次 Render 都遍历整个 snapshot 为全部目录构造请求，Sync 在共享锁内重建 map/set 并做路径键处理；每帧还无条件 QueueFolderSizeSort，导致没有新尺寸数据时，也会在后续 750 ms 节流到期后重新构造全量 keys 和 is_sorted 检查。鼠标悬停或动画重绘同样进入此链路。750 ms 仅限制后续重排，不限制每帧的请求准备。

全目录尺寸用于正确排序是必要的；让每帧重复提交和排队则不必要。建议以快照代次、排序模式、目录范围及 FolderSizes 变化为脏标记，绘制只读已有结果。验收比较无新数据时重复帧的请求重建次数，并保持离屏目录参与排序、异步尺寸更新和选择稳定。**重复工作由源码确认，未量化当前机器的帧耗时。**

### 6. P2：导航历史继续叠平行栈，依赖补偿函数维持一致性

位置：[app_model.h:44](../src/app/app_model.h#L44)、[app_model.h:245](../src/app/app_model.h#L245)、[app_model.cpp:65](../src/app/app_model.cpp#L65)、[app_model.cpp:78](../src/app/app_model.cpp#L78)、[app_model.cpp:530](../src/app/app_model.cpp#L530)。

原有路径栈、搜索来源栈之外，本轮又加入 viewport 栈，前后方向共六个容器。每次前进/后退/导航都需同步 push/pop；AlignHistoryOrigins 和 AlignHistoryViewports 发现长度不一致时会整组清空并补 null，只检查长度而不能证明语义对齐。增加功能时继续复制对齐补丁，会扩大维护面。

建议在局部边界内合为 HistoryEntry{path, search_origin, viewport} 的前后栈，旧数据缺字段只在恢复入口补默认。保留当前导航语义，覆盖搜索往返、路径恢复、滚动恢复和旧会话输入。**这是明确的结构债务，未发现当前正常路径已发生错位，不应写成已复现数据错误。**

### 7. P3：慢扩展的 deferred 策略已经不生效，计数与 UI 持久化仍在

位置：[context_menu_prefs.cpp:155](../src/app/context_menu_prefs.cpp#L155)、[context_menu_prefs.cpp:172](../src/app/context_menu_prefs.cpp#L172)、[app_main.cpp:1525](../src/app/app_main.cpp#L1525)。

恢复慢扩展可用性是正确修改。但生产路径已无 ComDeferred 调用或 deferred 调度读取，成功查询达到 500 ms 时仍增加计数并返回 changed，随后在 UI 消息处理中保存完整偏好文件。标记本身的测试继续通过，不能证明存在真实的延后调度功能。

最小建议：保留旧配置迁移所需读取，取消无效自动策略及重复持久化；若仍要延后调度，先明确并接入其消费点。不要为了清理恢复自动禁用。**读取链与写盘调用静态确认，本轮未测写盘延迟。**

### 8. P2：性能工具的 p95 与 PASS 文案容易造成错误验收

位置：[index_perf_main.cpp:106](../src/bench/index_perf_main.cpp#L106)、[index_perf_main.cpp:141](../src/bench/index_perf_main.cpp#L141)、[index_perf_main.cpp:154](../src/bench/index_perf_main.cpp#L154)、[index_perf_main.cpp:161](../src/bench/index_perf_main.cpp#L161)。

Percentile 采用 floor(p*(n-1)) 的下取整顺序统计量，五次样本的 p95 实际是第四小值；单次 diagnose 也仍打印 p50/p95。表格 PASS 只根据时间，而最终 enforce 退出码还考虑 error/incomplete，可能出现行内 PASS、整体质量失败。当前报告已明确披露测量限制，但工具输出仍会误导后续使用者。

建议定义明确的分位数规则并打印样本数、min/median/max；单样本标 single，质量与时间预算分别输出，保留各次原始数据。小样本分位数本身不是数学错误，问题在于它被不加说明地用于速度验收。**本轮只读核对公式与分支，没有重跑性能。**

### 9. P3：回滚优化后留下无效诊断，旧对照实现需要退出边界

位置：[index_engine.h:110](../src/index/index_engine.h#L110)、[index_host.cpp:448](../src/index/index_host.cpp#L448)、[index_engine.h:418](../src/index/index_engine.h#L418)、[index_engine.cpp:1774](../src/index/index_engine.cpp#L1774)、[index_storage_pair_test.cpp:67](../src/bench/index_storage_pair_test.cpp#L67)。

index_changed 已没有任何赋值，却继续输出并被测试断言为 false，是回滚旧方案后留下的无效指标。另有生产永不启用、仅 friend 对照测试打开的旧分卷查询路径；这部分目前有明确对照用途，不能直接当作无用代码删除，但应在迁移验收后安排移出生产编译或限定保留期。

先清除假指标或定义真实测量含义；旧分卷对照单独清理，保留旧文件格式读取兼容、WAL、恢复及回退必要逻辑。Engine::Search 的授权回调仍用于文件夹统计，不应因为文件名 Host 去掉鉴权就连带删除。**符号与生产调用链静态确认。**

### 硬编码、重复轮子与过度设计的判断

- 本轮检查的生产改动没有发现把用户的 D:/AppData/Index 或 E:/Projects/pulse 固定写为运行目录。此类路径主要出现在隔离测试、现场维护脚本和证据里；本机维护脚本有意固定目标并校验哈希，不能直接拿作通用部署脚本。问题 4 才是它们影响回归复现的实际边界。
- 菜单超时、worker 空闲时限/次数、缓存上限和索引格式常量有协议或资源约束用途；有数字不等于有害硬编码。只有跨位置维护同一语义、没有归属或没有消费点的常量/策略才值得集中。没有证据支持把它们全部改成配置项。
- 7-Zip 通过既有 Shell 扩展、复用共享 PIDL、每处理器保留 STA/COM 所有权、复用 worker 要求清理 ACK、调用过命令不复用，复杂度有隔离和实测收益依据。不是为了抽象而增加的框架。
- v12 posting 差分、basename interner 复用既有查询布局；旧格式兼容与 A/B/WAL 也有恢复用途。未发现值得另造通用压缩框架、替换整个索引引擎的依据。
- 编辑框 WM_SETREDRAW/WS_VISIBLE 的保持措施对应真实 Windows 行为。当前复用 mouse-selection 命名可读性一般，但本轮未证明因此产生功能缺陷；应与真正的重复业务工作和残留策略分级处理。

### 建议顺序、验证边界与停止条件

优先修复 1、2 的契约边界，并收好 3、4 的正式回归入口；再处理 5 的绘制热路径和 8 的性能输出。6 适合作为独立的小范围模型整理。7、9 可作为明确的残留清理任务，不能混入全仓重构。

1、2 验收应加入能先失败的冷菜单/挂载别名回归；3、4 以正式目标可复现构建运行为准；5 用重复帧不重建请求及无变化不重排的观测证明，不能只凭编译；6 保留导航兼容；7 保留显式禁用与旧配置迁移；8 保留真实错误和 incomplete；9 保留恢复兼容。相关检查通过即停止，不顺手扩大重构范围。

本轮没有运行新的产品构建或功能测试；只核对源码、调用链、已有验证失败记录及文档差异。原生菜单命令执行、USB 目录挂载、路径栏物理拖选、真实 UAC、裸程序名启动仍不能记为本轮运行验收。

## 路径栏与其他编辑框、动画闪烁追加检查

用户报告路径栏拖选与此前重命名相似的闪烁。本轮修复了共用原生编辑更新期间的短暂隐藏缺陷，并已安装；尚不能证明它是用户现场路径栏闪烁的全部原因，也未录制真实鼠标连续拖动的视频。

### 已确认与修改

- `DefPresentedChildEditProc` 在尺寸、选区、键盘等消息前后直接发送 WM_SETREDRAW FALSE/TRUE。原生 EDIT 会去掉 WS_VISIBLE；此前只有鼠标拖选事务重新保留可见样式，其他原生更新仍能短暂丢失子框表面。真实父绘制下，筛选展开动画也进入此路径。
- 现在这些短更新复用已有的保留可见样式事务，继续抑制原生绘制，由 LumaText 完成最终呈现。捕获期间不嵌套重置事务；保留外部隐藏、焦点、选区、原生回退和销毁清理。未更改动画时长或强制关闭动画。
- 覆盖路径栏、地址搜索、筛选、文件重命名、标签重命名，以及使用同一 ChildEdit helper 的输入框。路径栏拖选加父绘制的隔离用例在修复前已经通过，故不能把本次共同缺陷夸大为已复现路径栏拖选本身。

### 定向验证与保留的失败

- 新夹具连接生产对象、真实 HWND/EDIT 与 LumaText，五种编辑角色 × 深浅主题 × 100/150/200% 缩放，包含展开帧、稳定帧、拖选、真实尺寸改变、原生选区与键盘消息。最终 313 PASS / 0 FAIL。
- 私有基线仅还原本次两行产品修改，其余对象和断言相同：原生尺寸消息隐藏 30 项、选区/键盘隐藏 30 项、展开帧隐藏 6 项失败；另有全局前台窗口相等断言 1 项失败，合计 246 PASS / 67 FAIL。全局前台窗口可在长测试中由用户改变，最终夹具改为记录初末窗口及 PID，检查没有测试窗口留在前台。原始失败日志保留，没有将该前台变化归为产品缺陷。
- 首次夹具旁缺少 lumatext.dll，实际进入原生回退，保留 attempt-native-fallback.log；补齐与生产同哈希运行库后继续。早期只调用布局的 223 项通过，证明不能仅凭重复 SetWindowPos 源码就认定它是可见闪烁根因。
- 原生输入/回退定向目标 85 PASS；原有重命名拖选、捕获清理、长文件名和 Filter Enter/Esc 回归 22 PASS。检查实际渲染的 held-selection 位图：选区、文字均可见，未裁切；这是静态渲染证据，不能当作物理显示连续帧验收。
- 只构建 pulse 与 pulse_child_edit_test，主程序仅重编 edit_host.cpp；未全量回归。载荷检查、git diff --check 通过。

### 扩查其他动画

- 主窗口续帧使用不擦背景的失效方式；DirectComposition target 在 native 子框之后；操作中心以整张离屏位图呈现。未发现与本次相同的反复隐藏路径，但不代表全部动画视觉验收通过。
- 菜单搜索框每帧定位、弹窗淡入与独立子框透明度不同步、设置数值弹窗父区域擦除是代码疑点，均未取得可见闪烁的运行证据，未凭疑点修改。快速预览的查找框已有几何不变抑制重绘；全局搜索主要在窗口大小/DPI变化时布局。
- 仍需在真实显示器确认：用户路径栏长按拖选、弹窗前 150 ms 淡入、设置数值弹窗跨 DPI 的连续显示效果。本轮没有注入全局鼠标或更改用户设置。

### 安装状态

- 新 Pulse.exe SHA256：`C313945F5827CB72741484413BA2D4A114F4BF7D807CC6330878206CC7E536E1`；完整安装备份 `E:\Projects\pulse\dist\hosted-edit-flicker-20261023-213021`，69 个原有文件核验，包含上一轮仍驻留映像的恢复副本。只替换主程序，索引服务 PID 1100 全程保持 Running / Auto。
- 首次维护因主程序仍驻留而安全停止，未覆盖任何程序。普通权限未看到真实窗口；随后管理员枚举实际进程，通过已有托盘 Exit 处理器正常退出，成功更新后用普通用户重新打开。未强杀主程序或服务。
- [最终定向验证](../bench_data/hosted-edit-flicker-20261023/verified.log)、[修复前对照](../bench_data/hosted-edit-flicker-20261023/exact-before.log)、[安装结果](../bench_data/hosted-edit-flicker-20261023/deployment-result.json)、[原重命名回归](../bench_data/hosted-edit-flicker-20261023/rename-regression.log)。

## 文件名搜索策略更新（用户明确授权）

按用户“不要逐个文件鉴权”的明确要求，文件名搜索现在直接使用索引元数据。删除候选 ACL 检查、目录复验、最终页复验和相应 2 秒授权预算；总数与全局排序覆盖本次索引匹配全集。可搜到无权打开的文件名，实际打开、预览、读取和文件操作仍受其原有 Windows 权限约束。

保留索引服务连接身份验证、配置写入授权、原始索引目录 ACL、内容搜索及其他非文件名接口的现有边界。本次没有更改索引格式，不需要重建索引。

已更新安装版 Pulse.Index.exe；其余安装文件的哈希未变，68 个原有文件均完整备份并核验。服务恢复 Running / Auto；原主界面 PID 与启动时间保持一致。原网络代理仍运行旧映像，下一次正常启动使用新程序；因该映像占用产生的精确哈希旧文件保留，未强制结束代理。

| 查询 | 更新前单次 ms | 更新后中位 ms | 更新后样本 p95 ms | 完整匹配数 | 结果质量 / 预算 |
|---|---:|---:|---:|---:|---|
| a | 2077.40 | 367.01 | 405.37 | 1,249,754 | 完整；预算未通过 |
| ab | 2118.33 | 46.11 | 50.68 | 97,115 | 完整；预算未通过 |
| abc | 2181.50 | 1.31 | 2.71 | 4,014 | 完整；预算通过 |
| report | 2102.24 | 1.22 | 1.23 | 3,738 | 完整；预算通过 |
| pdf | 2188.85 | 0.92 | 1.92 | 4,233 | 完整；预算通过 |
| 无匹配 | 23.43 | 1.47 | 1.67 | 0 | 完整；预算通过 |

更新前每词仅 1 次，更新后每词 5 次，为同一真实安装服务管道的测量，包含查询、计数、排序、分页和序列化，不包含 UI 绘制。五次样本的分位值不代表稳定长期 p95；索引代次、后台负载与缓存状态未冻结。所有更新后响应 error=0、incomplete=no。`a` 需要对约 125 万匹配项排序，仍超过 150 ms；`ab` 的样本 p95 略高于 50 ms。因此速度验收退出码为 1，不能记为全部预算通过。

- 定向候选缓存/取消/并发/名称池及本次权限边界共 62 PASS / 0 FAIL。隔离文件被旧权限策略拒绝且 Windows 拒绝内容读取，但名称、索引大小、完整计数和分页正常返回。搜索不授予内容权限。
- 首次新增断言误以为拒绝属性位后 Windows 属性打开一定失败；实际当前令牌仍可打开属性句柄，内容读取被拒绝。保留首次 60 PASS / 1 FAIL 日志；修正该测试前提，增加真实令牌经旧策略拒绝的断言，未删减原有测试。
- 原有主机测试目标因缺少 transport 与 folder-size 的直接头文件无法编译。未修改原测试或弱化断言；私有驱动只补入两份头文件后包含原文件，11 条原断言全部通过，包括 16 连接、快速查询合并、最新请求、空结果、畸形帧和停机。原目标构建失败日志继续保留。
- 生产载荷检查与 git diff --check 通过。未执行全量历史回归，也未把后端测量当作主 UI 视觉验收。

恢复备份：`E:\Projects\pulse\dist\filename-index-only-20261023-210737`。新索引程序 SHA256：`908A7A4455468DA65135FF90E7214241AC7C42F9073D71B97AA19BA2E47E22ED`。

[测量和部署证据](../bench_data/filename-index-only-20261023/)；[定向回归](../bench_data/filename-index-only-20261023/search-cache.log)；[实际服务更新前](../bench_data/filename-index-only-20261023/installed-before.log)；[实际服务更新后](../bench_data/filename-index-only-20261023/installed-after.log)。

**下文逐候选鉴权、约 2 秒且不完整的结论是策略更新前的历史测量；当前文件名搜索以上节为准。**

## 本轮任务收尾与当前安装状态

**本轮确认问题的源码修复、索引清理及安装更新已完成；搜索速度目标仍未达成，真实微信调用、物理热插拔及第三方命令执行不记为已验收。** 此节为当前状态；其后各轮记录保留当时的结论和失败证据。

### 已完成的改动与安装

- 已安装回收站“清空回收站”按钮、重命名拖选闪烁修正、最外层“以管理员身份运行”、Filter Enter/Esc 字符消费、7-Zip 单选及多选菜单修正。真实清空回收站、对用户文件永久删除和实际 UAC 菜单命令未执行；相应生产处理器及隔离渲染/消息验证见历史节。
- 持久索引排除可移动设备与所有 USB 外接存储，包含 Windows 识别为固定磁盘的 USB 硬盘/SSD；当前目录临时扫描仍保留。真实 exFAT 历史后端 29 项通过；本次设备已不在线，无设备、取消和过期结果用例继续有效，未伪称物理拔插验收。MTP 手机尚无对应后端。
- 微信问题保留独立实验性 Explorer 接管开关，隔离选择/确认交付 9 项通过；当前用户开关仍为关闭。没有自动开启或改变用户偏好，也未捕获真实微信“在文件夹中显示”的调用链。#290 按误勾选排除。
- #374 使用自有 8 TiB 逻辑尺寸、0 字节分配的稀疏文件，取得真实 `0x80270037 COPYENGINE_E_RECYCLE_SIZE_TOO_BIG`。生产 OpsManager 经 Shell IPC 正确请求确切根的永久删除确认；拒绝后取消，原身份与标记保留。未确认本地有原报告同类缺陷，因此没有修改删除产品代码；未提交真实永久删除。
- 版本仍为 1.2.0.0；只替换实际改变的 Pulse、Index、Shell、Preview 四个程序，Document 未变。主界面经正常托盘“退出”结束后更新，未强杀用户界面；现已重新打开。服务已核验 Running / Auto，网络代理状态和完整安装/备份哈希见下方证据。

### 索引体积：已落实的两阶段优化

| 项目 | 初次现场 | 本轮最终现场 |
|---|---:|---:|
| 索引目录逻辑字节 | 2,335,240,520 B | 568,018,992 B（541.7 MiB） |
| 文件数 | 21 | 9 |
| aggregate A / B | v11，约 579.7 / 579.4 MB | 282,041,168 / 282,067,824 B |
| 当前活动代 | v11 | v12，第 204 代，2,465,648 节点 |
| 活动代 UTF-16 名称池 | 首次 v12 优化前约 119.5 MB | 73,863,866 B |

累计减少 **1,767,221,528 B（75.68%）**。节点和 WAL 在不同现场时刻有变化，这不是冻结语料的纯压缩率。此处为 D 盘目录文件长度合计，非实际分配空间/RAM，也不扣除 E 盘保留的恢复备份。

- 第一阶段：停止读写并清理可由 aggregate 重建的分卷查询副本；有序 posting ID 改为桶内差分 varint。保留 aggregate A/B、manifest、拼音侧缓存与真实根目录 Volumes WAL。
- 第二阶段：相同 UTF-16 basename 共享名称池偏移，大小写、规范化差异、NUL 和未配对代理项均按原始代码单元区分。保持 v12 布局、节点 ID/顺序、父子关系、元数据、FRN 和卷检查点。全量及单卷重建的直接写盘、增量 Flatten 与离线转换复用同一去重核心。
- 验收发现全量重建曾把活动池重新写回约 119.6 MB；已补齐生产写盘入口并验证事务性提交、重复准备幂等和写盘失败的共享名称 heap 维护。没有只做一次性删除来掩盖收益丢失。
- 冻结的现场 A/B 去重转换各有 15 项检查与 357 组查询对照；manifest 活动大小/CRC、A/B 有效性、停服期间真实 WAL 字节及原目录 ACL 均校验。最终列表在维护临时文件清理后重新取得。
- 现场名称统计：2,464,031 节点中有 1,113,594 个不同 basename，重复出现 1,350,437 次；该冻结样本的池从 119,499,040 降至 73,815,926 B。转换进程峰值工作集曾约 849 MB，包含映射/转换/验证，并非字典独占内存。
- UTF-8 或前缀编码仍为模型：现场去重加无损 UTF-8 的名称池模型为 38,411,674 B，尚未实施新格式或测得其运行代价。Everything 的已存数据库为 105,619,478 B，但数据范围、时点、字段和 A/B 策略不同；未声称读取了其未公开二进制结构，也不能承诺同容量。

### 右键菜单与第三方扩展

- 同一选择的 PIDL 每会话只解析一次；保留各处理器自己的 STA/COM 所有权和隔离进程。最多缓存 1 个已完整清理且从未执行命令的 worker，空闲 30 秒或满 8 次查询退休。超时、缺清理 ACK、调用过命令、可见窗口或异常 worker 均不复用；菜单对象每会话重建。
- 卡巴斯基首次 QueryContextMenu 仍有约 0.8 秒进程内初始化成本，同进程新 STA 可以受益。旧样本曾进入 MToolExtend，不能据此认定用户开启了其菜单模块；当前样本未捕获该模块，具体历史间接入口未确定。未改杀毒、美化模块、注册表或自动禁用设置。
- 安装版 Shell 的单文件完整查询为 **812 → 125/125/125 ms**；TXT+ZIP 多选为 **813 → 109/109/219 ms**。各次都有 17 行 7-Zip 项、16 个可用动作，完整 22 行有序菜单一致；每组复用同 PID，测试 worker 最终残留 0。这里只是 query/close，不包含主 UI/初始 broker 启动和第三方 Invoke。

### 搜索：落实的优化与未达成的目标

- 原始候选缓存现用于真实带权限过滤的查询，按代次/范围失效，命中缓存后仍重新授权。取消或明确截止在构造下一条路径前停止；普通权限拒绝继续扫描。query mutex 只覆盖候选缓存，其他只读查询可推进。
- 搜索在同一引擎共享快照内完成授权、排序和组装。撤销了“授权期间释放共享锁，任意 epoch 变化后清空整页”的尝试：持续无关元数据更新应等待当前响应，不能使已有效结果清空。该直接回归已通过；共享快照仍可能使写入等待权限 I/O。
- 增加只含聚合数值的匹配/路径/权限/排序/锁等待诊断。不会缓存跨响应的允许结论、跳过 ACL 或把 48 行当作精确总数。

最终实际安装服务单次诊断如下。每词仅一次，不能当作统计 p95；`incomplete=yes` 的数量不是精确总数，退出码 0 也不是速度预算通过。

```text
index items: 0

query       p50(ms)   p95(ms)   matches   returned   error   incomplete   budget
a           2074.83   2074.83      6724         48       0          yes   FAIL <= 150.00 ms
ab          2083.70   2083.70      4850         48       0          yes   FAIL <= 50.00 ms
abc         2147.38   2147.38      2272         48       0          yes   FAIL <= 50.00 ms
report      2092.33   2092.33      2228         48       0          yes   FAIL <= 100.00 ms
pdf         2164.54   2164.54      1793         48       0          yes   FAIL <= 100.00 ms
pulse-no-match-9f64b73ad180     6.47      6.47         0          0       0           no   PASS <= 100.00 ms
```

**常见词依然约 2 秒且结果未完成，50–150 ms 目标未通过。** 无匹配路径为毫秒级，不能据此量化全部阶段。`index items: 0` 是 StatusPayload 主动隐藏机器总条数的既有行为，并非空索引。保留逐候选新鲜权限检查与完整排序/计数语义后，候选缓存不能消除权限 I/O；进一步按排序先返回首屏、后台补统计，需要明确的部分计数/订阅续算语义和对应验证，当前没有冒充已经实现。

只读阶段探针取得并核对普通 Medium 用户令牌，在 2,465,388 节点快照上完成 36 组样本，全部 error=0、目录复验通过。5 个非空词的 10 次真实权限样本均返回 48 行但未完成，耗时 2.084–2.249 秒。

| 阶段 | 10 次非空权限样本的合计耗时占比 |
|---|---:|
| 权限回调 | 92.62% |
| 目录快照复验 | 4.42% |
| 当前页最终复验 | 2.23% |
| 候选匹配 | 0.189% |
| 路径构造 | 0.445% |
| 排序/选择 | 0.033% |

占比按各阶段微秒数合计除以相同样本墙钟合计，非单样本百分比平均。此组锁等待为 0；缓存命中的真实权限样本匹配约 77–150 μs，授权仍吃满既有预算，因此不能把微秒级匹配宣传为总体搜索延迟。探针不重放 WAL，不包含在线拓扑、后台更新、IPC 或主 UI，不能替代上方实际服务结果。

### 实际验证、恢复与验收边界

- 最终缓存/并发/名称写盘夹具 **48 PASS / 0 FAIL**；名称池维护通过独立 driver 复用原 fixture，**60 PASS / 0 FAIL**；元数据权限 24 项、worker 复用 25 项、超时 6 项、真实 context session 28 项、集成 127 项均保留各自日志。不同轮次及复用用例不相加为一个虚假总数。
- 只构建受影响的生产目标与定向夹具，未运行完整 release/selftest。旧 `pulse_index_engine_test` 目标因仍引用已删除的 `PreserveOfflineVolumesLocked` 而编译失败，日志保留；未删除或弱化其断言，相关名称池用例用独立最小 driver 运行，不将完整旧目标记为通过。安装版使用隔离目录和私有配置完成隐藏启动/渲染/正常退出，截图已检查，未见裁切或重叠；该截图不是所有新增交互的人工验收。
- 安装目录原有 68 个文件（包含保留的历史恢复文件）的最终期望哈希，以及 68 个初始完整备份哈希复核通过。初始完整备份：`E:\Projects\pulse\dist\finish-all-20261023-195135`；最后一轮程序与停服索引备份：`E:\Projects\pulse\dist\finish-all-20261023-201502`。v12 回滚需成套恢复对应程序、快照与 WAL。
- 延迟清理的旧映像：`absent`。未强杀用户网络代理；进程、启动时间、服务、最终文件列表与恢复后的 UI 另有记录。
- 最终在线目录清单只记录长度；运行中的 WAL 不做共享冲突下的哈希。WAL 字节一致性使用停服维护证据。最终核验脚本最初遇到在线 WAL 共享冲突、随后遇到 Windows PowerShell 5 对 hashtable 的 Measure-Object 兼容问题，两次失败记录均保留，修正采集与汇总方式后完成核验。
- 仍需真实用户场景才可验收：微信菜单调用、当前不在线 USB 的物理拔插、真实鼠标/IME 重命名观感、系统提示音听验，以及第三方菜单实际命令。MTP 是未实现能力。未确认的 issue 按原选择和证据保留，不因“完成所有任务”擅自修复推测问题。

证据：[最终安装及目录核验](E:/Projects/pulse/bench_data/finish-all-20261023/final-inventory-result.json)；[最终维护与回滚](E:/Projects/pulse/bench_data/finish-all-20261023/final-pass/maintenance-result.json)；[安装菜单查询](E:/Projects/pulse/bench_data/finish-all-20261023/installed-menu-result.json)；[安装版渲染](E:/Projects/pulse/bench_data/finish-all-20261023/installed-smoke.png)；[搜索与名称池](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/search-finish.json)；[集成验证](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/integration-finish.json)。

**下文各轮“未部署”“未清理”“线上收益未知”等为当轮历史状态，当前安装和现场容量以上节为准；历史测试及失败记录继续保留。**

## Everything 体积、卡巴斯基菜单与搜索性能追加调查

**当前轮次已落实原始候选缓存、超时/取消遍历停止，并完成定向验证；没有部署到安装版。** 本节同时区分旧版美化扩展栈和当前“未开启菜单功能”的调查结果。前轮索引清理及 v12 安装结果见下一节；前轮菜单解析复用修复仍未部署。

### 1. Everything 为什么更小

| 对照项 | Everything | Pulse |
|---|---:|---:|
| 本地版本 / 快照 | 1.4.1.1032 | v12 活动快照 |
| 文件与文件夹总数 | 2,519,325 | 2,463,927 |
| 单份保存文件 | 105,619,478 B（100.73 MiB） | 327,561,632 B（312.39 MiB） |
| 约字节 / 项 | 41.92 | 132.94 |
| 目录合计 | 此处只计一份 everything.db | 659,009,579 B，含恢复用 A/B 与其他文件 |

Everything 条数来自官方 SDK 的空查询，仅请求总数、返回行数为 0，没有读取文件名列表。它是 17:48 的运行时数量，DB 最后保存于 14:54；Pulse 数量来自已记录的活动快照，因此不是同一冻结数据集的精确逐项对照。但两者都约 250 万项，不能再仅以数量不同解释约 3.1 倍的单文件差距。

**本机保存配置是 `db_compress=0`。** Everything 支持可选 Bzip2，但这次 105.6 MB 不能归因为本机开启了这一外层压缩。保存配置可能与尚未保存的运行时设置有差异；没有强制保存或重建 Everything 数据库。其文件大小与修改时间已重新核对。

官方作者在 2020-11-04 说明名称用 UTF-8，记录父目录引用、大小和修改时间；内存中另有名称/路径/大小/时间排序结构，普通子串搜索使用优化后的多线程 strstr。历史数据库文档还公开了排序记录中的 code length/code offset，以及长度 0 复用名称。作者在 2020-11-10 明确说这份文档对 Everything 1.4 已过时、整体结构仍相似。因此可以学习紧凑编码与名称复用，不能把旧表偏移当成本机 1.4.1.1032 的已验证格式，也不能断言其当前磁盘文件不包含任何派生结构。

本节更正前轮调查边界：前轮所查索引/INI/FAQ 页面没有提供二进制布局；本轮找到了官方历史格式页面和作者的版本限定。旧证据保留，但“没有公开格式”不应理解成网上不存在官方历史文档。

| Pulse v12 活动快照段 | 字节数 | 占单份快照 |
|---|---:|---:|
| UTF-16 文件名池 | 119,496,382 | 36.48% |
| 双字符倒排 | 57,148,528 | 17.45% |
| Node | 39,422,832 | 12.04% |
| FRN | 39,422,832 | 12.04% |
| 单字符倒排 | 32,647,376 | 9.97% |
| Attr | 29,567,124 | 9.03% |
| 子项顺序 | 9,855,708 | 3.01% |
| 卷记录、头和对齐 | 850 | <0.01% |

Pulse **已经使用 basename + 父节点编号**，不是每项都保存完整路径。Node 16 + FRN 16 + Attr 12 = 44 字节，再加子项顺序 4 字节，固定部分即 48 字节/项；名称池本身也已大于 Everything 整个 DB。单/双字符倒排合计 89,795,904 B，已经过上一轮差分 varint 优化，仍是另一个明确成本。A/B 恢复代使整个目录约为两份快照，但不能用移除恢复代来掩盖单份编码差距。

下一轮体积优化按下列顺序评估，均尚未实现：

1. **名称池：**先统计 ASCII/UTF-8 长度分布、重复 basename 和公共前缀，再比较 UTF-8、名称字典、分块前缀编码。中文 UTF-8 未必比 UTF-16 小；不能预先承诺减半。名称解码、大小写/Unicode 匹配、随机访问、重命名和 WAL 都必须一起验证。本轮准备了只输出聚合量的统计器，但尚未运行。
2. **固定记录：**评估 FRN/父节点编号、默认属性和时间/大小的分列或分块编码，保留稳定文件身份。固定部分已占 118,268,496 B；不能为了节省字段破坏重命名、增量更新或恢复。
3. **派生候选结构：**测量更小的 posting 编码，以及哪些排序/倒排可以仅驻内存、启动时重建。把结构从磁盘移到启动计算会增加启动 CPU/内存成本，不等同于免费缩小；也不能据旧文档推断 Everything 当前的完整策略。
4. **恢复策略：**保留 A/B；若未来改成更小的前代/增量恢复，必须单独验证断电、损坏、WAL 重放及版本回滚。本轮不删除恢复快照。

官方来源：[作者的索引算法说明](https://www.voidtools.com/forum/viewtopic.php?p=60222)、[历史 DB 格式](https://www.voidtools.com/support/everything/db/)、[可选索引](https://www.voidtools.com/support/everything/indexes/)、[INI 设置](https://www.voidtools.com/support/everything/ini/)。
[Everything 本地与格式证据](E:/Projects/pulse/bench_data/everything-size-20261023/evidence.json)；[官方页面时间与哈希](E:/Projects/pulse/bench_data/everything-size-20261023/official-sources.json)。

### 2. 卡巴斯基首次成本与美化菜单关闭状态

已核对 CLSID `{36d7ad11-6890-4adb-b2da-c74d182cdb7c}`，注册 DLL 为 Kaspersky 21.26 x64/shellex.dll，版本 21.26.4.406。真实 QueryContextMenu 的 vtable 方法地址也落在该 DLL 中；没有仅凭注册项名称归因。

**下表及 11 次栈采样属于 17:46–17:52 的旧现场，当时 MToolExtend 为 2.0.2.0；不能把旧版本观察直接当成当前开关状态。**

| 隔离对照 | 实测结果 |
|---|---|
| 新进程首次 QueryContextMenu | 约 1,166–1,270 ms |
| 同一目标线程 CPU | 常规冷调用约 1,047–1,203 ms；另一采样冷调用约 1,031 ms |
| 同进程换新对象 / 原对象重复 | 约 1.4–4.8 ms |
| 同进程依次换新 STA 线程 | 1,219.69 → 1.94 → 1.63 ms |
| 先生成 7-Zip 菜单 / 先创建 Shell.Application | 随后的卡巴斯基冷调用仍约 1,212 ms |
| 返回菜单项 | 本次探针样本为 0；不代表所有场景都不应有菜单 |

14 组对照改变单/多文件、目录、菜单标志、Initialize 的目录/关联参数和 ShellFolder 数据对象，均未消除首次成本。Initialize 本身约 0.001 ms。WCT 采样没有观察到跨线程等待链，线程 CPU 又接近墙钟时间，所以**不能把这一秒解释成已经证实的阻塞式 IPC 等待**；采样仍可能遗漏短暂等待。

64 帧有限调用栈的 11 次采样均含卡巴斯基 shellex.dll → Windows.Storage，其中 2 次还记录到内部嵌套的 combase / KERNELBASE / ntdll → MToolExtend.dll → ucrtbase。MToolExtend.dll 对应“枫の美化工具箱扩展”2.0.2.0。两个指令采样也观察到该模块。可以确认它参与了慢调用链；样本不足以将全部 1.2 秒分摊给卡巴斯基、Windows 或美化扩展，未证明注入方式，也未定位到第三方源码级根因。

Pulse 每次菜单使用新的隔离进程，会重复付出这次进程内首次初始化成本。可行方向是**在隔离宿主中有界复用进程/空闲预热**：保留一个受控 STA，菜单对象仍逐会话创建关闭；进程卡死可终止，保留会话所有权、取消、当前用户令牌和资源释放边界。新 STA 对照证明同进程可保留热态收益，但还不是生产宿主复用验证。不能把扩展移进 UI 线程，也不能把超时自动禁用当成性能优化。

前轮 PIDL 只解析一次的修复解决约 1 秒的重复 Shell 解析；这轮调查解释剩下的另一段首次成本。前轮最终共享构建中 7-Zip 首次结果约 323–353 ms，全部扩展完成约 1.53–1.56 秒。两者是不同阶段，不重复累计为同一个根因。安装版 Shell 尚未替换；这轮没有禁用杀毒、美化扩展或任何处理器，也没有执行扩展菜单命令。
[新 STA 对照](E:/Projects/pulse/bench_data/kaspersky-latency-20261023/new-sta-same-process.jsonl)；[有限栈摘要](E:/Projects/pulse/bench_data/kaspersky-latency-20261023/stack64-summary.json)；[矩阵运行记录](E:/Projects/pulse/bench_data/kaspersky-latency-20261023/run-results.json)。
[卡巴斯基完整结构化证据](E:/Projects/pulse/bench_data/kaspersky-latency-20261023/evidence.json)。

**针对“没有开启右键菜单功能”的追加核实：**

- 当前 MToolExtend 已于 18:50 更换为 2.1.0.0，本机另有 MToolFluentMenu.dll 1.0.0.2。官方发布说明将 Fluent 上下文菜单作为单独可启用功能列出。通用扩展出现在调用链里，不能推导出这项功能已打开。
- 旧探针的 production 模式只显式 CoCreate 卡巴斯基处理器，没有枚举并创建 MTool 处理器；旧栈证明的是卡巴斯基查询内部间接进入 MToolExtend，不是 Pulse 单独选中了美化菜单。具体历史加载入口仍未知。
- 当前 HKCR 64/32 位的即时 CLSID InprocServer32，以及 Pulse 对本次 TXT/ZIP 选择覆盖的 26 个 ContextMenuHandlers 路径，均未找到指向 Extend 或 FluentMenu 的匹配；选定的其他全局 Shell 入口也无匹配。此范围不覆盖所有手动加载、钩子或间接入口，不能由无注册匹配断言 DLL 不可能加载。
- 只做一次当前版本的新进程对照：首次 QueryContextMenu **767.04 ms**（目标线程 CPU 718.75 ms），同进程重复 **1.72 ms**。7 次有限栈样本均有卡巴斯基/Windows.Storage，未捕获两个 MTool 模块；探针退出和清理正常。首次成本在当前样本中仍存在，尚无受控开关 A/B，不能把 1.2 秒到 0.77 秒的差异归因于升级或关闭菜单。
- 配置检查只读取了 config.mcfg 的格式和元数据，未输出私人配置内容、未读取账号令牌；其为专有 dmresv2 格式，未解码出菜单开关。因此保留用户“未开启”的现场说明，不声称独立读取到了该布尔值，也不修改设置进行试验。

[关闭功能追加核实](E:/Projects/pulse/bench_data/kaspersky-latency-20261023/mtool-disabled-followup.json)；[配置检查边界](E:/Projects/pulse/bench_data/kaspersky-latency-20261023/mtool-config-inspection.json)；[官方发布页面取证](E:/Projects/pulse/bench_data/kaspersky-latency-20261023/mtool-release-source.json)。

### 3. 搜索提速应先改结果生成流程

**先纠正验证结论：此前工具只读取结果数量，没有解析回复末尾的 error/incomplete。** 因此旧日志只能证明服务回复成功，不能证明结果完整。本轮已完善 `pulse_index_perf` 的结果记录解析、质量列与 `--enforce` 判定，并新增单次 `--diagnose` 和无匹配词；实际路径/文件名不输出。没有据此宣称原来所有 25 次回复都已查明完整性。

| 本轮实际安装服务查询 | 墙钟 ms | 可见计数 | 当前页 | error | incomplete |
|---|---:|---:|---:|---:|---|
| a | 3,724.51 | 7,200 | 48 | 0 | 是 |
| ab | 2,217.83 | 5,418 | 48 | 0 | 是 |
| abc | 2,192.96 | 2,064 | 48 | 0 | 是 |
| report | 2,100.78 | 2,177 | 48 | 0 | 是 |
| pdf | 2,197.71 | 1,747 | 48 | 0 | 是 |
| 唯一无匹配词 | 7.29 | 0 | 0 | 0 | 否 |

每词仅 1 次诊断采样，不能当成统计意义上的 p95；工具虽然保留 p50/p95 栏名，在此模式两者只是同一观测值。常见词均未满足性能预算，且结果不完整，表中计数不能当成精确总数。无匹配词 7.29 ms 表明 IPC 和空结果路径并非始终需要两秒，但不能单凭这一对照量化匹配算法各阶段的开销。

优化前源码确认的开销来源（前两项优化已在下文落实，其余成本仍保留）：

- `index_host.cpp::SearchThread` 在进入引擎前创建 2,000 ms 的 CallerVisibility；在引擎等待锁、匹配期间预算也会流逝。返回前还会重读目录快照，并用新的权限检查器复核当前页。
- `Engine::Search` 先取得全部候选，对每个候选构造完整路径并执行授权，之后才排序并选 48 行。权限检查包括父目录链、读取安全描述符、AccessCheck，以及模拟请求用户再次打开。它会真实触碰文件系统。
- 授权预算过期后，Visible 会拒绝并标记不完整；但 MatchSet::ForEach 不能退出，而调用顺序仍先 BuildQueryPathLocked，再进入已超时的回调。过期查询还会继续遍历/拼接大量路径，取消时也只跳过回调而未退出遍历。
- 引擎已有原始候选缓存，但仅在没有 authorize 回调时填充；真实服务搜索带回调，所以连续输入不能正常利用这一填充路径。安全的优化目标是缓存未授权候选 ID，不能缓存前次允许访问的结论。
- query_mu_ 与引擎共享锁覆盖了上述权限 I/O；存在阻挡其他查询与更新的代码路径。本轮尚未量化并发排队占比。

**本轮已落实：**完整原始候选在授权前缓存，真实服务查询也能复用；每次响应仍重新授权。缓存按查询文本、目录范围、是否只查文件夹、索引代次和节点集合失效。超时信号与普通权限拒绝分离；只有取消或预算结束才真正停止遍历，并在下一次拼路径之前退出。缩小查询时同类的 lambda 取消问题也已修复。超时保持既有 incomplete 语义，不把不完整计数当精确总数。

回归覆盖不同授权回调、同调用者重复响应、被拒绝条目仍保留于 raw 集、范围切换、同节点数的属性变更、空结果失效、分页/排序，以及稀疏与 70,000 候选的真实遍历退出。测试回调隔离不是新建 Windows 账号的跨用户实测。

独立目录夹具对 10,000 个拥有访问权的文件执行现有元数据可见性检查，10,000 个均可见，耗时 1,188 ms，未超时，夹具清理通过。这证实权限路径本身就可能昂贵，但不是现场索引阶段耗时分解，更不是一次通过的延迟预算测试。

**与 Everything 比速度时还必须对齐可见性约定：**官方作者说明 NTFS 索引忽略文件权限，官方服务文档也说明本地用户可获得全部 NTFS 文件名列表。Pulse 当前按请求用户过滤可见结果，做了额外工作。此结论仅指 NTFS 文件名列表；不意味着 Everything 能绕过打开/读取文件的系统权限，也不泛化到内容搜索或 folder indexing。不能通过删除 Pulse 授权检查来宣称等价提速。

| 优先级 | 改进方向 | 收益与验证边界 |
|---|---|---|
| 已实现 | 为候选遍历增加明确的取消/预算停止信号，在拼路径之前停止 | 消除已经超时后的无效工作，保留 incomplete；不得把普通权限拒绝误判为停止信号。不会自动解决正常的两秒授权成本。 |
| 已实现 | 缓存原始候选集，按索引代次、查询及目录范围失效 | 连续输入复用候选；每次仍重新授权，不复用允许结论。单独做这一项预计不能消除逐项授权瓶颈。 |
| 随后 | 内存匹配/排序和权限 I/O 分阶段，缩短持锁时间 | 用稳定对象身份与代次校验处理变化；需验证取消、重命名/删除、并发更新及权限变更，不能直接解锁后继续引用 mmap 指针。 |
| 主要体验改善 | 按目标排序先授权足够的可见行，先显示当前页，再补充分批结果/统计 | 不必为显示 48 行先授权所有候选；精确总数改成下界/处理中是协议和 UI 语义变化，需要明确标识，不能显示为精确总数。保留发布前权限复核。 |
| 最后度量 | 优化字符匹配、SIMD/并行、posting 结构与路径重建 | 先取得匹配、权限、排序、锁等待的独立计时，再按占比投入。当前未证明更换子串算法能解决约两秒的现场延迟。 |

原始候选缓存、授权遍历停止及缩小查询取消已实现并验证；缩短持锁时间、分阶段首屏和其他算法调整尚未实施。本轮没有替换安装版索引服务，不能用合成缓存样本承诺现场达到 50 ms，预算内的逐候选权限成本仍在。

[实际服务与质量标志](E:/Projects/pulse/bench_data/search-latency-20261023/installed-before-quality.log)；[独立权限夹具](E:/Projects/pulse/bench_data/search-latency-20261023/visibility-fixture-before.log)；[改进后的诊断工具](E:/Projects/pulse/src/bench/index_perf_main.cpp)。
权限对照来源：[官方作者 2019-03-15 回复](https://www.voidtools.com/forum/viewtopic.php?p=25276)、[Everything Service Security](https://www.voidtools.com/support/everything/everything_service/)。

### 本轮实际验证与限制

- 本轮 pulse_index_search_cache_test **29 PASS / 0 FAIL**；70,000 候选合成样本冷查询 1,852 μs、缓存查询 5 μs，仅说明该隔离样本的候选复用效果，不是现场总体搜索延迟。新 pulse_index 与安全夹具目标均构建成功，未部署。
- 新增 --metadata-acl-only 入口复用已有元数据安全用例，普通 Medium 令牌 S-1-16-8192 下 **24 PASS / 0 FAIL**，覆盖父目录撤权、子项属性拒绝、目录快照失效、恢复及超时，夹具清理通过；不运行无关队列、追踪或 raw-tree 测试。初次沙盒运行设置 ACL 返回 Win32=5，保留全部失败记录；同一组用例未删除、未弱化。
- 新目标初次因 NMake 尚未再生成规则而不可用，随后只重新生成既有构建目录。服务初次链接又遇到旧 index_folder_sizes 对象引用旧 Search 签名；显式重编该对象及另外两个依赖 Engine 布局的实现对象后，受影响目标构建通过。未做全量清理或全量构建。
[本轮搜索优化与构建证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/search-optimization-outcomes.json)；[普通用户 ACL 回归](E:/Projects/pulse/bench_data/search-latency-20261023/metadata-acl-after-medium.log)；[初次沙盒失败记录](E:/Projects/pulse/bench_data/search-latency-20261023/metadata-acl-after.log)。

- 已定向构建 pulse_index_perf 与 pulse_index_security_test，完成实际服务 6 个词的单次诊断和 --visibility-bench；未运行完整安全套件、全量 release/selftest 或实际 GUI 搜索验收。诊断检查错误与完整性后显示的是性能/质量失败，没有以退出码 0 替代通过结论。
- 只读阶段探针已构建，设计为读取实际 v12 快照，分别测无授权、只构造路径和真实用户授权；不启动引擎、不写快照或 WAL、不返回名字。
- **读取受限快照的阶段探针尚未形成有效数据。** 旧 Windows 管理员请求现已结束，但输出文件为 0 字节，不能算执行成功；没有捕获其退出码，也未确定具体失败阶段。没有绕过 ACL 或把未取得的阶段数据作为实测，因此尚不能给出现场每阶段精确比例。备份目录同样拒绝当前读取，未尝试修改权限。
- 卡巴斯基调查使用私有探针和受控文件，未改杀毒/美化配置、注册项或安装文件；没有调用第三方实际菜单命令。新对象/新 STA 的热态收益是探针证据，不能当作已完成宿主复用。
- 唯一完整报告继续为本文；各 JSON、源码和日志为证据。早期状态仍保留在后面的历史节中，冲突处以本节的新增实测与限定为准。

## 现场索引清理、压缩与菜单延迟最新结果

**截至本次最终复核：现场索引清理完成，Index 服务程序已更新；主程序与 Shell 安装文件仍为原版。** 下列容量均为文件逻辑字节数，不是磁盘实际分配空间或常驻内存。

| 项目 | 初次现场清单 | 完成后 |
|---|---:|---:|
| 索引目录总计 | 2,335,240,520 B（2.175 GiB） | 659,009,579 B（0.614 GiB） |
| 文件数 | 21 | 9 |
| aggregate A/B | v11，579,699,360 / 579,404,816 B | v12，327,574,736 / 327,561,632 B |
| v9/volumes 派生查询副本 | 四卷、每卷 A/B 与 manifest | 已删除，服务重启后未重建 |

初次清单到最终清单累计减少 **1,676,230,941 B，约 71.78%**。最终清理阶段精确回收派生副本 **1,164,971,712 B**。维护期间服务正常发布新快照、重置已归并 WAL，现场节点数也有变化；总差值不是同一冻结数据集上的纯编码压缩率。真实 WAL 在停服维护期间逐文件校验不变，重启后的正常追加/归并另计；未按名称删除根目录 `Volumes`。

### Everything 对比与本轮实现

阅读了 voidtools 官方索引说明、INI 的 db_compress 和 FAQ，并保留网页文本。公开可确认的是：Everything 运行时数据库驻内存，文件名/路径必需、部分元数据可选；磁盘数据库支持 Bzip2 压缩，读写时以 CPU 换 I/O。官方没有在这些资料中公开可供本轮核验的 everything.db 二进制记录布局，因此未把推测当成已读到的结构。Everything 的约 100 MB 用户观察值与 Pulse 的节点、元数据、A/B 恢复份及覆盖范围未经对齐，不能承诺优化到相同容量。

- [Everything 索引说明](https://www.voidtools.com/support/everything/indexes/)、[db_compress 官方设置](https://www.voidtools.com/support/everything/ini/)、[FAQ 资源示例](https://www.voidtools.com/faq/)。 本地原始页面位于 `bench_data/index-compaction-20261023/*-official.txt`。
- Pulse 现场 v11 活动快照中，单字符和双字符倒排段合计 **343,075,792 B，占 59.18%**。本轮将有序节点编号改为桶内差分 varint，并保留桶偏移；查询直接解码选中的桶，沿用 mmap。没有采用整库 Bzip2 解压到堆内存的运行方式。
- 同时停止生产模式写入和读取派生分卷查询副本，查询使用 aggregate。主快照 A/B、卷身份/FRN、元数据、卷检查点、拼音侧缓存及真实增量 WAL 保留。v12 读取器兼容 v7–v11，v11 不因纯编码版本升级强制全盘重扫。旧程序不读 v12，回滚必须同时匹配程序与快照/WAL。
- 冻结的 60,050 节点夹具单份快照从 **15,865,312 → 8,675,344 B，减少 45.32%**。严格加载校验样本约 **0.82 → 4.79 ms**；查询中位数相近。这是定向样本，未作冷缓存、常驻内存或所有查询加速承诺。

[压缩实现及来源证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/snapshot-postings-compaction.json)。

### 实际验证、安装及恢复

- 压缩定向检查 **45 PASS / 0 FAIL**，包含 **1,071** 组旧/新编码查询比较：节点/属性/文件名/FRN/卷检查点逐段一致，全部 posting ID 逐桶一致，Unicode 与代理对重命名、增删、真实 WAL 字节一致和重放、v12 损坏主代回退 v11 上一代。存储检查 **42 PASS / 0 FAIL**，另外包含 **291** 组查询比较及停止采用遗留副本的恢复边界。
- 现场每代转换各 **15 PASS / 0 FAIL**，各有 44 组实际快照查询对照。最终再次读取两代文件，确认 v12、节点非空、活动 manifest 字节数与 CRC 一致；派生副本未重建，原索引根 ACL 不变。服务恢复 Running / Auto，原网络代理 PID 10816 保持。
- 全部 40 个安装文件前后逐项核对，只有 `Pulse.Index.exe` 改变；最终 SHA256 为 `91E3F7E96EE374B6AB9081CD152F9E2A9033A86BB0EAE5AB0A8B1D59859BFE5D`。未结束驻留主程序、未修改用户设置或第三方插件注册表。
- 维护脚本的 Windows PowerShell 5 数组、退出码和原生 NULL 绑定问题均保留失败证据。前两个保护检查发生于转换前；原生 NULL 问题阻止无备份名的 ReplaceFileW 发布，后经隔离真实文件替换验证修正。期间 Index 二进制已成功更新，服务恢复后自动发布了 v12 快照；最终从核实状态重新完整备份并成功清理，未将中间失败记为通过。
- 首轮存储验证因 NMake 遗留的 index_shard 对象仍使用 v11 常量而失败。显式重编三个目标的相关对象后，45/42 项验证通过。未运行完整 release/selftest 或旧版整套索引测试；后者存在机械降版本夹具与历史符号引用需要另行适配，未弱化这些测试。

最终完整回滚位置：`E:\Projects\pulse\dist\index-maintenance-20261023-04`。含全量安装文件与停服时全部原始索引，逐文件哈希验证；最终又复核 21 个索引备份文件。先前原版程序与 v11 索引备份另保留在 `dist/index-maintenance-20261023-02`。备份位于 E 盘并继续占空间，上述节省指 D 盘现场索引目录，不是所有磁盘合计释放量。

[现场维护结果](E:/Projects/pulse/bench_data/index-live-cleanup-20261023/maintenance-result.json)；[最终原生检查](E:/Projects/pulse/bench_data/index-live-cleanup-20261023/final-check.json)；[安装文件核对](E:/Projects/pulse/bench_data/index-live-cleanup-20261023/installed-payload-after.json)。

### 实际服务搜索：功能返回，性能预算仍未达标

更新前后均向实际服务进行 a / ab / abc / report / pdf 各 5 次查询；25 次回复均成功，p95 更新前约 2,102–3,881 ms，更新后约 2,108–3,755 ms，仍全部超过工具的 50–150 ms 预算。工具未启用 --enforce 时退出码 0 不代表性能通过。本轮不宣称搜索加速；现场数据会变化，结果数不同也不能直接用于同数据集等价证明。状态显示 count=0 是服务 StatusPayload 主动隐藏机器总条数的既有协议行为，不是空索引；实际快照约 246 万节点且查询有返回。

[更新前搜索](E:/Projects/pulse/bench_data/index-live-cleanup-20261023/query-before.log)；[更新后搜索](E:/Projects/pulse/bench_data/index-live-cleanup-20261023/query-after.log)。

### 右键菜单慢：两段原因及修复状态

生产并发跟踪确认，21 个扩展工作线程分别 SHParseDisplayName 解析同一选择，7-Zip 线程的路径解析约 **987 ms**，它自身 QueryContextMenu 仅 **0.63 ms**。改为每菜单会话解析一次不可变 PIDL，工作线程仍各自创建和持有自己的 STA/COM 对象；全部线程创建后统一放行，修复早期混合选择样本在工作线程创建阶段的停顿。没有保留生产打点代码。

| 选择 | 基线首次 7-Zip / ms | 最终共享构建首次 7-Zip / ms | 最终全部扩展 / ms |
|---|---:|---:|---:|
| 单文件 | 1,211 | 353 | 1,562 |
| 多文件 | 1,308 | 331 | 1,537 |
| TXT + ZIP | 1,312 | 323 | 1,528 |

私有最终宿主 10 项、共享构建复核 4 项均通过；保留 7-Zip 父项与 16 个原命令及全部基线行，提前关闭无错误，测试宿主残留 0。仅实际 ShellClient 查询和关闭，未执行压缩命令或人工验收弹出菜单。

最终回复仍约 1.5 秒，因为 CLSID `{36d7ad11-6890-4adb-b2da-c74d182cdb7c}` 仍超过 1 秒。本机只读注册表确认其为 **卡巴斯基 Kaspersky 21.26 的 x64/shellex.dll**。已有解析复用阶段 trace 中该处理器 QueryContextMenu 约 1,058–1,329 ms；最终共享构建仅确认它超过 1 秒，未把早期精细打点数值当成最终逐段实测。协调器持续发布部分结果，最终结果等待所有线程；IPC 另有 100 ms 轮询带来的小段尾部成本。没有禁用卡巴斯基或更改其配置，也没有证据解释该 DLL 内部为何慢。

**右键修复目前是已构建验证的源码，未部署到安装版。** `Pulse.Shell.exe` 共享构建 SHA256 为 `557846B94CE8D4F021E81539F28B46FD7C4F91C41BD98473BB3E2F60FFCB9D01`；安装版 UI 与 Shell 仍运行原版本，不能声称用户现用菜单已达到表中速度。此前 7-Zip 自动禁用迁移/最外层入口等 UI 改动也仍需后续受控更新。

[共享 Shell 复核](E:/Projects/pulse/bench_data/menu-latency-20261023/shared-build-check/validation.json)；[并发根因跟踪](E:/Projects/pulse/bench_data/menu-latency-20261023/validation.json)；[卡巴斯基注册信息](E:/Projects/pulse/bench_data/menu-latency-20261023/remaining-handler-registration.json)。

以下保留各轮审计和验证的历史记录；“未部署”“未清理”等状态仅适用于其所在轮次，当前现场状态以上节为准。

**修复前审计快照：168 条候选，19 条排除，149 条进入故障或待判定清单；16 条定向运行复现、27 条静态确认、27 条已有针对实现（未运行验收）、10 条定向未复现、66 条缺少条件、3 条行为已确认但缺陷未定。** 这些数字保留历史审计口径，不是修复后的剩余缺陷数量。重复 issue 保留编号；混合报告只确认明确列出的子项。

**首轮修复 18 组已确认的本地子问题，关联 25 条 issue；不是宣称这 25 条原报告的所有现象均已消除。** 首轮相关定向回归共 226 条断言通过；完整 Shell/IPC 图标加载尝试仍有 1 项前置失败，微信真实调用链也尚未验收。后续勾选项及新增问题的处理见追加核验。安装版未替换。

## 首轮修复结果

修改以本地源码直接证明或修复前夹具复现的子问题为边界。表中的通过数属于相应整组夹具，同一夹具覆盖多个条目，不重复累计为新测试。

| 编号 / Issue | 实际改动 | 定向验证 | 限制 |
|---|---|---|---|
| F01 / #59 | 仅最终COM菜单快照进入跨会话缓存。 | Shell29/0；有修复前失败对照。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-shell.json)） | 原第三方扩展/完整桌面菜单未验收。 |
| F02 / #250、#248 | 忽略末尾 -Embedding 等开关，避免将其作为目录。 | 命令行22/0；实际隐藏主程序前后启动对照。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-main.json)） | 仅参数误当路径子项。 |
| F03 / #49 | 普通打开逐文件提供其父目录为工作目录。 | Shell29/0，含真实私有批处理。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-shell.json)） | 完整UI/IPC双击链未验收；UNC仅参数验证。 |
| F04 / #235 | 文件拖出允许COPY/MOVE/LINK。 | UI22/0；实际CF_HDROP、本地LINK-only COM目标。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-ui.json)） | 原PDF软件和外部OLE拖动未验收。 |
| F05 / #106 | 回收站列宽调整使用渲染的同一列宽数组。 | UI22/0；回收站及搜索视图对照。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-ui.json)） | 隐藏窗口合成输入。 |
| F08 / #213 | 保留Shell缩略图alpha类型，RGB忽略无意义alpha。 | 真实DIB经生产WIC转换4/0。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-preview.json)） | 原PSD/7Z样本、第三方provider/IPC未验收；仅RGB边界。 |
| F09 / #197 | 恢复窗口矩形约束到当前显示器工作区。 | 矩形10/0，含实际只读显示器查询。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-navigation.json)） | 未物理拔屏或移动用户窗口。 |
| F14 / #86 | CreateProcess错误740时以原程序、参数、目录重试runas。 | Shell29/0，API拦截覆盖740、取消与非740。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-shell.json)） | 未启动实际提权程序或HiBit。 |
| F16 / #372 | Medium/Large图标格边缘预留4DIP框选起点。 | UI22/0，图标中心为选择对照。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-ui.json)） | 未验收物理指针整段框选。 |
| F17 / #188、#187、#186、#185、#183 | 清空搜索取消请求、清除延迟历史并返回原目录。 | 导航31/0，含四组合、迟到结果、composing守卫。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-navigation.json)） | 只修复清空搜索子项，未覆盖各混合报告全部现象。 |
| F18 / #136 | 长行内编辑保留水平视口，重复点击不漂移。 | UI22/0，caret182→182。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-ui.json)） | 未验证真实IME或前台物理输入。 |
| F19 / #169、#129 | 后退/前进保留两轴滚动，异步发布及选择恢复保持视口。 | 导航31/0，Details2168→2168、LargeIcons1632→1632。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-navigation.json)） | 真实私有目录worker；非安装版人工验收。 |
| F20 / #236、#87 | 网络侧栏固定项提供取消固定菜单。 | UI22/0，取消和取消固定均验证。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-ui.json)） | 虚构UNC且不持久化；不删除网络索引根。 |
| F23 / #173 | 最后一列外边缘支持调整，移动增量只应用一次。 | UI22/0，三次移动共缩小30px。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-ui.json)） | 隐藏窗口合成输入，未覆盖物理拖动/多DPI。 |
| F24 / #334 | 窄盘名采用单行字符省略号。 | UI22/0；184/320px截图目视。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-ui.json)） | 浅色100%DPI；过长末尾盘符仍可被省略。 |
| F25 / #229 | 通知/显式刷新使本地图标缓存失效，拒绝迟到回复。 | 生产worker定向7/0。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-preview.json)） | 完整Shell/IPC首次加载1PASS/1FAIL；未到图标替换步骤，不算整链通过。 |
| F26 / #263 | 相对路径在调用方目录内转绝对路径后再IPC转发。 | 命令行22/0，跨不同目录编码/解码。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-main.json)） | Listary未引号空格参数拆分尚未确认。 |
| F27 / #58 | 目录总量参与大小排序，保持选择/视口，交互期间延期。 | 真实目录统计排序30/0；三策略×两方向及节流/取消。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-preview.json)） | 仅普通物理目录，未扩展搜索/压缩包/回收站。 |

F06、F07、F10、F11、F12、F13、F15、F21、F22、F28、F29、F30 保留审计结论，未实施修复：涉及缺样本/设备、原现象未复现、产品约定未定或跨协议能力扩展。F10/F21等已确认能力限制仍存在，不能称全部已确认条目均已修复。

### 微信“在文件夹中显示”

- 只读检查发现，当前安装版为 `D:/Applications/Pulse/Pulse.exe`，版本 `1.2.0.0`；实际配置 `D:/AppData/Pulse/app.json` 中 `experimental_explorer_takeover=false`。普通文件夹、Win+E、此电脑的偏好值为开启，但偏好值不能证明注册表中的有效关联已正确生效；本次相关注册表查询未取得足够证据。报告不复制包含私人历史的完整配置。
- 源码确认：后台仅在实验开关开启时启动 Explorer 窗口接管；设置页却没有对应字段、布局和命中入口，控制器也没有转发已有的 24 号动作。本轮恢复“接管 Explorer 窗口（实验性）”独立开关，沿用既有接管与关闭保护逻辑；当前用户配置仍为关闭。
- 设置验证 71 PASS / 0 FAIL：生产控制器动作、服务请求、视图模型与真实渲染命中；中英文、100%/200%、760/1100 DIP、深浅主题共 16 张截图已检查，新行无裁切重叠。使用内存注册表和禁用真实 Explorer 服务的隔离夹具，注册表写入为 0。
- 没有捕获微信本次具体使用的 API，也未执行微信菜单后完整接管。配置关闭足以解释既有窗口观察接管没有启动，但不能因此断言它是微信未接管的唯一原因，或声称微信问题已修复。新版构建尚未部署，安装版仍是此前程序。
[命令行与微信检查证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-main.json)。

### 首轮实际验证

| 范围 | PASS / FAIL | 原始日志 |
|---|---:|---|
| 命令行 | 22 / 0 | [日志](E:/Projects/pulse/bench_data/issue-fixes-main/cli-test.log) |
| Shell启动 | 29 / 0 | [日志](E:/Projects/pulse/bench_data/issue-fixes-main/shell-final.log) |
| 导航 | 31 / 0 | [日志](E:/Projects/pulse/bench_data/issue-fixes-navigation/final.log) |
| 窗口位置 | 10 / 0 | [日志](E:/Projects/pulse/bench_data/issue-fixes-navigation/placement.log) |
| UI | 22 / 0 | [日志](E:/Projects/pulse/bench_data/issue-fixes-ui/after.log) |
| 缩略图alpha | 4 / 0 | [日志](E:/Projects/pulse/bench_data/issue-confirm-preview-fixes/run-alpha.log) |
| 图标缓存 | 7 / 0 | [日志](E:/Projects/pulse/bench_data/issue-confirm-preview-fixes/run-icons.log) |
| 文件夹大小排序 | 30 / 0 | [日志](E:/Projects/pulse/bench_data/issue-confirm-preview-fixes/run-sizes.log) |
| 接管设置 | 71 / 0 | [日志](E:/Projects/pulse/bench_data/issue-fixes-main/settings-run.log) |

回收站功能区的“清空回收站”按钮已保留，并使用最终统一构建对象重新验证：441 checks / 0 failures，覆盖中英文、深浅主题、100%/150%、1100/640/380/300 DIP、空/非空/加载/未知占用、命中及实际指针消息入队。复用既有 EmptyRecycle 操作，工作线程保持停止，未执行真实清空。此441项为按钮功能复核，未计入上方修复回归226项。[按钮验证日志](E:/Projects/pulse/bench_data/issue-fixes-main/recycle-run.log)。

另外：实际主程序隔离隐藏启动 `-Embedding`，修复前打开 `E:/Projects/pulse/-Embedding`，修复后打开默认 `C:/`，两次正常退出。Shell/IPC 图标完整尝试的 1 PASS / 1 FAIL 单独保留，发生在初次取图标时，未到达缓存替换步骤；原因未定，不记为通过，也未推断为本次回归。

首轮 `pulse` 及受影响 Shell/Preview 目标构建通过，该轮主程序 SHA256 为 `66e3dbc9b48d1075784609707c174fb0e80c6ee44b7ff7b6d91f7fa62d203435`；受影响的命令行、Shell 启动及图标测试目标已构建。共享构建串行进行，私有夹具统一使用重建后的结构布局。没有运行完整 release/selftest，没有部署、切换真实接管配置、操作用户窗口或清空真实回收站。

## 勾选项与新增问题追加核验

原勾选 5 条中，用户明确 #290 为误勾选，已取消；本次选择为 #314、#308、#302、#374。另处理用户明确提出的重命名拖选闪烁、可移动设备策略、管理员菜单位置、筛选 Esc 提示音。勾选和已有针对代码均不替代本地故障确认。以下为追加核验，不改变首轮历史审计计数。

| 项目 | 实际处理与证据 | 验证边界 |
|---|---|---|
| 重命名拖选闪烁 | 修复前 16 次鼠标移动各触发一次原生重绘关闭/开启及 WS_VISIBLE 丢失。改为整次捕获拖动共用一次重绘抑制，保留原生实时选区和 LumaText 呈现；补齐释放、失捕获和失去按键状态的收尾。最终 16 次原生选区更新保留，移动期间重绘切换和可见状态丢失均为 0。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-user-followup-ui.json)） | 22 项使用实际生产处理器、原生 EDIT 和 LumaText，选区截图已检查；没有物理鼠标录像、真实 IME 或实际文件重命名验收。 |
| 可移动及 USB 外接存储；#314 / #308 | 统一持久索引设备策略，排除 DRIVE_REMOVABLE 及包括 Windows 固定磁盘在内的 USB 外接存储。旧开关、手动根和后台写入不能重新启用；设置页禁用对应选项并显示简短状态。复用现有工作线程扫描器保留已识别设备当前目录内文件名及内容临时搜索，分页、取消和 UNC 路由定向检查通过。旧离线/未知内置根保留配置和缓存，只暂停不允许的扫描。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/followup-removable.json)） | 追加真实 G:/ exFAT USB 后端检查 29 PASS / 0 FAIL，设备排除、临时搜索、取消、陈旧结果及隔离配置写入通过；证据见专项第9节。未实际拔盘或手工验收安装版 UI；MTP 手机不属于本次盘符后端。 |
| 以管理员身份运行 | 将已注册的直接 runas 操作提升至最外层“打开”之后、“打开方式”之前，保留偏好筛选、调用绑定、缓存与 COM 会话有效性约束。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/followup-menu.json)） | 15 项菜单组成/调用、27 项渲染、原打开方式 14 项及异步菜单 9 项通过；8 张中英文、100/150%、深浅主题截图已检查。没有真实提权或注册表修改。 |
| Ctrl+F 后 Esc 提示音 | FilterEditProc 消费已处理 Enter/Esc 后队列中的 WM_CHAR，阻止字符继续到达原生 EDIT 默认错误提示音路径。普通输入及同步逻辑保留。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/fixes-user-followup-ui.json)） | 包含于上述 22 项，未重复累计；未播放和听取实际系统音频。 |
| #302 索引体积 | 第一阶段源码已完成：同时停写、停读可由 aggregate 重建的分卷查询副本，保留主格式、A/B 和真实 WAL。6 万文件双卷双代隔离快照逻辑大小减少 50.8%；三组各 97 个查询对照及更新/恢复通过。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/storage-optimization-implementation.json)） | 安装版未替换，真实 2.16G 目录未清理。逻辑大小和映射长度不是实际分配空间/RAM；现场迁移及更广泛性能验收仍待完成。 |
| #374 大目录回收失败 | 生产 RunDelete 在隔离 IPC 注入 SIZE_TOO_BIG 后进入新的确切根永久删除确认。拒绝确认不发永久删除；接受才发新 token 请求；后端取消不产生虚假完成/撤销。普通访问拒绝不错误升级为永久删除。8 项通过，未发现该路径需要修复的本地同类故障。（[证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/followup-index-audit.json)） | 未改删除产品代码；未启动真实 Shell、填满真实回收站或删除用户文件。原目录/系统 HRESULT 未取得，不称原大目录场景已验收。 |

### 追加定向验证

| 范围 | PASS / FAIL | 原始日志 |
|---|---:|---|
| 管理员菜单组成与调用 | 15 / 0 | [日志](E:/Projects/pulse/bench_data/issue-followup-main/admin-menu-test.log) |
| 管理员菜单本地化渲染 | 27 / 0 | [日志](E:/Projects/pulse/bench_data/issue-followup-main/admin-menu-ui.log) |
| 原打开方式回归 | 14 / 0 | [日志](E:/Projects/pulse/bench_data/issue-followup-main/open-with-regression.log) |
| 异步实时菜单回归 | 9 / 0 | [日志](E:/Projects/pulse/bench_data/issue-followup-main/menu-live-regression.log) |
| 重命名拖选与筛选 Enter/Esc | 22 / 0 | [日志](E:/Projects/pulse/bench_data/issue-followup-ui/after.log) |
| 可移动设备策略与临时文件名扫描 | 36 / 0 | [日志](E:/Projects/pulse/bench_data/issue-fixes-removable/policy.log) |
| 应用搜索入口与设置渲染 | 23 / 0 | [日志](E:/Projects/pulse/bench_data/issue-fixes-removable/routing.log) |
| 离线内置盘配置与缓存保护 | 12 / 0 | [日志](E:/Projects/pulse/bench_data/issue-fixes-removable/cache.log) |
| 网络重叠根边界与回收错误码映射 | 2 / 0 | [日志](E:/Projects/pulse/bench_data/issue-followup-index/network-overlap-run.log) |
| 回收拒绝后的重新确认、拒绝与后端取消 | 8 / 0 | [日志](E:/Projects/pulse/bench_data/issue-followup-index/delete-confirm-run.log) |

追加定向检查合计 168 PASS / 0 FAIL，与首轮 226 项分别记录，不重复累计。

此前追加验证的构建记录（不重标为本次重跑）：追加修改后 pulse 与 pulse_index 定向构建通过（build-final-policy.log）；状态文字缩短后 pulse 再构建通过（build-final-labels.log），无新增编译警告。最终 Pulse.exe SHA256 `3404159d8d78e3f7d23f0e74e4d2ef755d858b74991703c33708c9a5691e8bed`；Pulse.Index.exe SHA256 `1e1093ffeb69553aa28fbdda2e55101d98bd4a064e6e00337b1248e3ea6aabee`。编辑器夹具使用其证据记录的统一编辑器对象；之后仅索引边界及资源重链，没有改变这些编辑器对象。未运行完整 release/selftest，未部署或替换安装版。 本次存储构建和最终索引程序哈希见下一节专项结果。

现场容量均为用户观察：D:/AppData/Index 约 2.16 GB；v9/9ACBEFD40DDAC9E8 与 v9/volumes 各约 1.08 GB；前者 base-a.bin 约 565 MB；everything.db 约 100 MB。并非工具读取的精确字节数，不将不同数据集合及索引结构的容量直接作为重复记录证明。目录哈希及两套表示的源码证据解释了占用结构，未核实现场各文件的活动代或文件身份。根目录 Volumes 的活跃增量 WAL 与 v9/volumes 查询副本不同，不能按相似名称清理。

尚待受控安装更新、真实索引清单与派生副本回收，以及报告明确列出的现场验收。#374 原大目录 Shell 现场返回值如以后可安全取得再比较；#290 按用户更正排除。本轮不再提问，不更改 ACL、所有者、真实配置或服务，不删除现存索引。

## 7-Zip 菜单消失追加修复

本节处理用户新增的“单选和多选时 7-Zip 菜单消失”。只修改已证实的偏好策略和菜单组成，不将此前大压缩包调用卡住的未决问题记为修复。安装版未替换。

**现场根因已确认：实际配置中 7-Zip 的项目开关为开启，但 CLSID 慢调用记录是 `timeout=3, deferred=true, disabled=true`。** 实际配置通过存储位置注册表只读定位到 `D:/AppData/Pulse/context_menu.json`，并非默认 AppData 目录。旧 `RecordComTiming` 把成功但超过 1000 ms 的回复累计为“超时”，第三次直接禁用；宿主又把已完成且耗时超过 1000 ms 的处理器报作 slow CLSID。因此插件即使正常返回，后续仍被跳过。[配置窄化证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/sevenzip-menu/preferences-narrow.json)。

另一层已复现的问题是所有第三方 Shell 菜单都被收进“更多选项”。现在将非背景选择的 7-Zip 子菜单放到最外层，在固定内置项之后；保留原始子命令、偏好筛选和当前会话绑定，迟到回复仍能更新已打开的菜单。其他软件入口保留。

慢调用只保留统计和 deferred 标记，继续使用已有异步加载，不再根据成功调用耗时自动禁用。配置保存版本升为 2；加载旧版本中 `disabled=true && timeout>=3` 的记录时在内存中恢复。明确的 `items[h:CLSID].enabled=false` 仍有效，版本 2 显式禁用可往返保存。旧版本同一条慢调用记录无法再区分自动禁用后又手动设 disabled 的操作，迁移按旧阈值证据恢复；明确的项目关闭不会被覆盖。

### 本机实际查询与耗时

隔离 ShellClient 查询本机注册的 7-Zip 26.04，使用专用微小夹具；只查询并关闭会话，没有执行压缩命令或改注册表。下列为本轮构建的独立 Shell 宿主端到端回调时间，安装版二进制哈希不同，不能把它视为安装版精确耗时。

| 选择 | 首次含 7-Zip 的回复 / ms | 所有扩展最终回复 / ms | 7-Zip 子命令 |
|---|---:|---:|---:|
| 单个 TXT | 1317 | 3069 | 16 |
| 两个 TXT | 1090 | 2397 | 16 |
| TXT + ZIP | 1447 | 2647 | 16 |
| 单目录 | 1087 | 2286 | 16 |

最终回复时间包含其他扩展；不能全归因于 7-Zip，也不是压缩执行时间。7-Zip 到达后即可显示，无需等待所有扩展结束。[查询汇总](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/sevenzip-menu/summary.json)。

另做一次独立分段测量，使用同一专用单文件夹具：

| 测量段 | 耗时 / ms | 边界 |
|---|---:|---|
| 生产 Shell 宿主启动、连接及 Ping 提交 | 66.94 | 不包含 Ping 回复往返 |
| 私有 STA 的生产 BindCtxSelection | 91.83 | 独立探针，非生产并发工作线程现场 |
| 7-Zip CoCreateInstance | 0.97 | 独立探针 |
| 7-Zip Initialize | 0.15 | 独立探针 |
| 7-Zip QueryContextMenu | 0.68 | 独立探针；HRESULT=18 为成功命令范围计数 |
| 生产 CollectHandlerItems | 0.05 | 独立探针；取得父项及 16 子项 |

这次独立测量中，7-Zip 自身生成菜单不到 1 ms。它不能解释上表生产回调首次出现的约 1.1～1.45 秒延迟，也不能把两组测量拼接相加。生产并发宿主内剩余耗时尚未归因；本轮没有宣称菜单延迟已经优化。自动禁用策略的确定缺陷可以独立修复，无须把耗时直接归咎于 7-Zip。

### 定向验证与限制

- 修复前同一回归夹具出现 15 条菜单断言失败、4 条慢调用策略断言失败；修复后策略 10 PASS、单选/多选/混合选择及真实 FluentMenu 刷新渲染 73 PASS、原异步菜单边界 9 PASS，均为 0 FAIL。缓存结果在当前会话验证前保持不可调用，验证后按新命令 ID 绑定。
- 用实际配置经生产 FromJson 解析、重放四类真实 Shell 回调的 172 条检查通过；恢复后不再向宿主传入 7-Zip 跳过 CLSID，每个含 7-Zip 的阶段均恰好有一个最外层入口及 16 个对应的有效会话命令。该夹具重放先前已取得的真实回调，并未冒充修复后重新完成安装版交互。[配置与回调对照](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/sevenzip-menu/composition-after-prefs-fix.log)。
- 生成 24 张深浅主题、100%/150% 的根菜单及子菜单截图，标签裁切检查通过；人工查看其中 4 张代表截图，未见裁切或重叠。截图子命令使用受控注入数据，不是安装版真实菜单截图。[单选根菜单](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/sevenzip-menu/single-100-dark.png)、[多选根菜单](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/sevenzip-menu/multi-150-light.png)、[子菜单](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/sevenzip-menu/multi-150-light-children.png)。
- 仅构建 `pulse` 和 `pulse_context_menu_loading_test`，无新增编译警告；最终 `Pulse.exe` SHA256 `646fbbd8dd64c46e49b2c40b00988f0355f511e8d58b7449850d828a556077dc`。同步更新已有 selftest 的旧策略断言，但没有运行完整 selftest/release。
- 实际配置 SHA256 前后相同：`96fc981897ab3dc39cc371bcc89b5a432c3d29ca2b865e32a167034e2a382948`。没有修改用户配置、安装目录、服务、ACL 或真实索引；现场恢复须使用更新后的程序加载配置。没有验证实际 7-Zip 命令执行或解决此前压缩包调用卡住的问题。

本轮索引优化进度仍为：停写停读派生分卷查询副本的源码及隔离定向验证完成，快照逻辑大小减少 50.8%；真实 exFAT U 盘后端 29 项通过。安装版未替换，2.16G 现场目录未清理；下一节保留详细说明。

## 索引体积优化专项评估

日期：2026-10-23。本节按用户要求展开 #302，现已完成第一阶段源码优化、构建、双路径对照及真实 exFAT U 盘后端验证。仍使用同一份报告。安装版与真实索引目录未替换或清理。

### 1. 结论与建议顺序

**实现前已经确认：同一个机器索引 Engine 保存 aggregate 总索引，再生成完整分卷查询副本，两类各保留 A/B 两代。现在生产默认停止生成和加载分卷查询副本，直接使用既有 aggregate 查询路径。** 这解决重复持久表示；不能据此认定存在重复扫描或重复活动结果。

**第一阶段已实现并通过定向验证：保留 aggregate、A/B 和真实 WAL。** 6 万文件双卷双代隔离快照逻辑大小由 64,517,120 降至 31,731,060 字节，减少 50.8%。现有旧副本保留且不再读写，所以用户当前 2.16G 目录尚未直接缩小。后续先做可审查的迁移回收，再评估单份快照压缩。

双套表示能够解释相当一部分占用，不能解释 Pulse 与 Everything 的全部差距。即使回收了一整套约 1.08G 副本，剩余一套仍显著大于用户观察的 100MB。单份快照内哪些结构占主要空间，仍需实际计量。

### 2. 当前掌握的容量，及尚未掌握的内容

| 对象 | 大小 | 证据性质 |
|---|---:|---|
| D:/AppData/Index | 约 2.16G | 用户观察，未取得精确字节和实际分配空间 |
| v9/9ACBEFD40DDAC9E8 | 约 1.08G | 用户观察；源码及独立哈希计算确认此目录名对应 aggregate |
| v9/volumes | 约 1.08G | 用户观察；源码明确用作分卷查询快照根 |
| 总索引目录中的 base-a.bin | 约 565M | 用户观察；尚未读取 manifest，不能指定 A 一定是活动槽 |
| everything.db | 约 100MB | 用户观察；未核对相同卷、排除规则、文件数量、属性和版本 |

普通及提权后的只读访问、精确已知文件路径的读取均遭 Windows 拒绝。失败没有换算为 0 字节，也没有修改权限或所有者。本轮没有再次尝试读受保护目录。`Get-Volume` 也返回拒绝访问，不能据此判断 U 盘是否已经插入。

特别需要取得但尚未取得的是：manifest 的 active_slot/previous_slot、两槽各自字节数与代际、每卷快照清单、临时/旧格式文件是否实际存在，以及单份快照内部各段的占比。目录名 v9 是当前存储布局；当前新写快照的头部版本为 11，不能因路径带 v9 就认定它是可删除的历史版本。

### 3. 为什么会有两组近似等大的目录

```text
同一份机器文件名索引集合
├─ v9/9ACBEFD40DDAC9E8/       总索引 aggregate
│  ├─ manifest.json          指定活动槽和上一代
│  ├─ base-a.bin
│  └─ base-b.bin
└─ v9/volumes/<volume-hash>/  每卷再生成一份完整查询表示
   ├─ manifest.json
   ├─ base-a.bin
   └─ base-b.bin

另有 Index/Volumes/<volume-hash>/wal-a.log 等真实增量日志，
它不属于上面的 v9/volumes 查询副本。
```

`AggregateShardPaths` 使用 source_id=aggregate，`ShardIdHash` 独立计算结果为 9ACBEFD40DDAC9E8。实现前的 实现前的 `WriteVolumeShards` 从总索引逐项复制 Node、Attr、名称和 FRN，调整节点编号后再次调用完整的 `WriteIndexFile`。因此不是只写少量卷元数据，而是第二套数据和检索结构。构建、合并和单卷恢复仍调用此入口；新默认策略在创建目录或写入前直接返回。保留旧实现仅供私有 friend 夹具比较，没有用户设置或环境变量启用入口。[目录映射](E:/Projects/pulse/src/index/index_engine.cpp:232)、[完整分卷写入](E:/Projects/pulse/src/index/index_engine.cpp:1713)、[哈希计算](E:/Projects/pulse/src/index/index_shard.cpp:98)。

`PublishShardBase` 发布到另一个槽，再把原活动槽记为 previous_slot；这两槽通常是不同时间的索引状态，不是两个同时活动的结果集合。上一代承担故障恢复用途，不能只因为名字是 base-b 就删除。[两槽发布](E:/Projects/pulse/src/index/index_shard.cpp:367)。

近似容量模型是：`总活动代 + 总上一代 + 各卷活动代合计 + 各卷上一代合计 + 日志及其他文件`。若两代大小接近、分卷集合与总集合一致，就可能接近四份单代数据。此处是结构模型，未证明现场四份文件均存在、相同大小或均有效。

### 4. 能减少多少，不能承诺什么

| 动作 | 预期效果 | 条件与限制 |
|---|---|---|
| 仅停止以后生成分卷副本 | 避免后续整套分卷写入、相应建表及发布工作 | 现有文件仍在，立即释放的磁盘空间为 0 |
| 停写停用后，验证并回收现有派生查询副本 | 按用户给出的分布，候选回收量约 1.08G，即当前总量约一半 | 必须确认清单、无旧进程继续使用、恢复和回滚条件成立；不是实测释放承诺 |
| 再删除 aggregate 上一代 | 可能再少一个快照 | 会改变现有恢复合同；不作为第一阶段方案 |
| 压缩单份快照内部结构 | 可能继续降低剩余体积 | 尚无分段实测，不能给压缩倍数或达到 100MB 的承诺 |

关闭分卷映射可能减少重复映射及相关内存压力，但映射文件字节不等于驻留 RAM。不能据此声称进程内存也会减少一半。当前诊断分别记录 aggregate 和 shard 的映射字节，后续应同时量 Working Set、Private Bytes 和峰值。[现有映射统计](E:/Projects/pulse/src/index/index_engine.cpp:370)。

### 5. 单份 565M 内部实际存了什么

当前 `base-*.bin` 是 PIDX 二进制文件，不是 SQLite。依据当前新写入格式，可按下面公式盘点一份快照：

```text
S ≈ 128 + 16N + 12N + 4N + 2L + 16F
     + 4(P1 + P2) + 8 × 65537 + 卷表 + 对齐
```

N 为节点数（包括文件夹），L 为名称池 UTF-16 单元数，F 为 FRN 映射记录数，P1/P2 为两种倒排表的节点 ID 总数。此公式描述当前写入代码，不能不读头部就套用到旧版本现场文件。

| 结构 | 当前存储方式 | 对体积的含义 |
|---|---|---|
| Node | 每项 16 字节 | 父节点、名称偏移和长度、标记及当前使用的辅助数据 |
| Attr | 每项 12 字节 | 大小和修改时间 |
| child_order | 每项 4 字节 | 父子/名称顺序访问 |
| 名称池 | 每 UTF-16 单元 2 字节 | 每个节点的名称，并非简单只算可见字符数 |
| FRN 表 | 每条 16 字节 | 文件身份、节点对应和 USN 更新所需 |
| 单字符与相邻字符对倒排 | 每次成员出现 4 字节，另有两组 65537 项起点表 | 一个文件名会属于多个不同检索桶，条数自然大于文件数 |

倒排构建已经对同一文件名内的同字符/同字符对哈希桶去重。把一个节点从其他有效桶里删掉，会改变搜索能力；这不是可以直接做的重复记录清理。当前写入器也已经把 name_order/size_order/mtime_order 三个全量排序数组置为不存在，不能把“再删除这三组数组”当作尚未实现的优化。[结构宽度](E:/Projects/pulse/src/index/index_engine.h:41)、[实际序列化与倒排构建](E:/Projects/pulse/src/index/index_engine.cpp:1478)。

需要先形成真实字节分解：nodes、attrs、pool、child_order、prefix1 IDs、prefix2 IDs、各自起点表、vols、FRN、对齐/文件尾、旧代/临时文件各占多少。再记录每节点字节、每名称平均长度及 P1/N、P2/N。只读盘点器不应调用可能在回退时重写 manifest 的生产恢复入口。

Everything 官方 FAQ 区分数据库磁盘大小与 RAM，并说明 1.4 默认索引大小、日期及额外排序信息；Options 文档也列出可配置字段和索引范围。因此这里不能把 Pulse 的目录总量、Everything 的单个 DB、某个内存字节估计混作相同口径。没有证据支持“Everything 只有名字，Pulse 多了属性所以体积差就合理”，也没有证据支持在相同能力下必然能缩到 100MB。官方资料标识和用途见本节结构化证据。

### 6. 为什么优先保留 aggregate

| 用途 | aggregate | 分卷查询副本 |
|---|---|---|
| 启动加载和主状态恢复 | TryLoadCache 的入口 | 没有从这些副本重建完整主状态的现成路径 |
| 全局节点、FRN、卷 journal/USN 状态 | AdoptMappedLocked 从这里建立 | 依赖已存在且匹配的 aggregate |
| 重命名/删除覆盖层、增量回放、单卷恢复基线 | 仍由主状态维护 | 不替代这些状态 |
| 查询 | 有完整现存路径 | 匹配时可替代部分节点、属性、名称和倒排读取 |
| 映射关系 | 主映射持续保留 | 所有卷匹配后附加映射；未替代主映射 |

新默认策略先清空已有分卷映射和 ready 标志，再于任何路径创建、manifest 解析前返回。旧实现仍由私有夹具启用，要求总索引已存在且各卷匹配，失败时回退 aggregate。`QueryNode/Attr/Name` 与倒排选择沿用原有 aggregate 路径。主索引节点、FRN/USN、授权过滤和协议保持原实现。[映射入口](E:/Projects/pulse/src/index/index_engine.cpp:1936)。

仍然不能据此跳过性能对照：总表与分卷表的访问局部性、跨卷和限定目录查询的成本可能不同。现有分卷排序流还有依赖排序数组的分支，而当前写入器不生成这三组数组；旧文件是否仍携带以及实际会使用多少，也需实际头部及运行证据。

### 7. 分阶段实施建议

**阶段一已完成：减少整套派生副本，保留主格式与恢复。** 统一私有策略默认 false，同时控制 WriteVolumeShards 和 RefreshQueryShardsLocked。只在测试 friend 内启用旧路径作对照；不增加用户选项。未更改快照版本、全局 ID、A/B、真实 WAL、USN 或 feed 协议。运行结果见下节。

**阶段二：独立处理现存派生文件迁移。** 新程序停止依赖这些副本后，也不会自动释放已有 1.08G。应先用只读文件清单验证相对路径、大小、manifest、当前/上一代完整性和进程使用情况；在受控更新窗口释放映射，确认总索引启动、增量和回退可用，再处理清单内被证实为派生查询副本的文件。保留在同一磁盘的改名备份不算已回收空间；最终回收与回滚保留期必须分别记录。权限不足、清单不完整或恢复验证失败就停止回收。

这里应防止误删 Index/Volumes 的活跃增量日志；它与 Index/v9/volumes 不是同一角色。不能使用通配“所有 volume 目录”的清理规则，也不能以清空整个 Index 目录代替迁移。[真实增量日志路径](E:/Projects/pulse/src/index/index_delta.cpp:63)。本轮没有执行任何现存索引删除，详细迁移方案应在实际执行前形成可审查结果。

**阶段三：按占比压缩单份快照。** 若倒排 ID 占主要空间，可比较递增 ID 的差分/变长或分块编码；若名称池占主要空间，再评估名称字典/共享和其他编码；若 FRN/固定记录占主要空间，再评估字段布局。收益都需实测。这类修改会影响现有 mmap 直接数组访问、版本兼容和边界校验，不能与阶段一混成一次大改。删除名为 unused 的字段也不可凭名字决定，当前代码在使用它。

| 备选方向 | 取舍 | 建议 |
|---|---|---|
| aggregate 为主，停用派生分卷快照 | 利用既有查询路径，保留格式及恢复 | 本轮已实施；定向结果见下节 |
| 分卷为唯一真源，删 aggregate | 需重做全局节点/FRN/WAL/启动及跨卷恢复组织 | 不作为本轮起点 |
| 先压缩全部 base 文件 | 随机访问/映射、解码、版本和错误恢复复杂度增加 | 先计量，再选局部结构 |
| 先删除上一代 | 空间收益直接，但削弱恢复 | 不采用作第一阶段方案 |

### 8. 本轮实际验证与测量

专用 `pulse_index_storage_test`：**42 项顶层检查 PASS / 0 FAIL，退出码 0**，其中包含 baseline、overlay、WAL-restart 三组各 97 个查询比较。数据为两个合成 MFT 卷、6 万文件、48 个目录，未扫描真实内置磁盘。旧模式明确断言两个分卷映射生效，新模式断言无分卷映射。[原始日志](E:/Projects/pulse/bench_data/index-storage-optimization/paired-run.log)。

| 检查 | 实际结果 |
|---|---|
| 命中与分页 | 独立算术核对总数及 alpha/report/TXT 数量；比较完整列表、名称/大小/时间/索引顺序正反序、页切片、过滤、范围、排名、授权回调与取消 |
| 更新与重启 | 合成 USN 触发真实目录移动、文件重命名/删除/新增；实际逐卷 WAL 刷盘后通过 TryLoadCache 重启回放，节点路径和各卷 USN 游标通过 |
| 旧副本与新写入 | 全新输出从未生成 v9/volumes；带有有效/陈旧分卷 manifest 的旧输出可加载，新模式分卷映射为 0，发布后旧副本所有文件长度及 CRC 均不变 |
| 恢复 | 两模式都保留总索引两代；破坏活动快照后回退上一代，拒绝不匹配的新代 WAL，并进入已有恢复状态 |

| 相同数据的测量口径 | 旧模式 | 新模式 |
|---|---:|---:|
| 一代快照逻辑字节（含 manifest） | 32,259,224 | 15,865,748 |
| 两代快照逻辑字节（含 manifest） | 64,517,120 | 31,731,060 |
| 活动总索引映射长度 | 15,865,312 | 15,865,312 |
| 活动分卷查询副本映射长度 | 16,389,792 | 0 |
| 两次合成建树及发布耗时 | 630.797 / 639.363 ms | 328.597 / 372.523 ms |
| 基线查询 median / P95 | 12.872 / 33.242 ms | 13.799 / 35.244 ms |
| 覆盖层查询 median / P95 | 21.027 / 42.084 ms | 19.473 / 40.741 ms |

空间为快照文件逻辑长度，未计真实 WAL、分配簇和临时峰值。映射长度不等于 Working Set 或 Private Bytes，没有据此宣称 RAM 减半。查询各模式/阶段先预热 3 次，再计时 30 次，轮换 alpha、*.txt、record 的名称排序首屏/分页查询，并清理 Engine 查询缓存；OS 文件缓存没有清空，旧模式先测。这一组 P95 分别约 +6.0% 和 −3.2%，没有观察到超过拟议 10% 的回退，但不是全面性能保证，也不能代替大规模安装现场基线。两次发布耗时不是整盘扫描耗时。

`pulse_index`、`pulse` 和专用测试目标均构建成功，无新编译警告。主程序 SHA256 保持此前值；索引程序 SHA256 为 `b460630d23cff6c0d5b624dff9aa5ed570739e9c64199db734b713bfa311a0e2`。没有运行完整 release/selftest；没有将历史测试标成这次重新通过。[结构化结果与哈希](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/storage-optimization-implementation.json)。

### 9. exFAT U 盘实际结果

**真实 G:\ exFAT U 盘，29 PASS / 0 FAIL。** Win32 识别为 DRIVE_REMOVABLE，生产探测在线且 removable_bus=true，STORAGE 描述符 BusType=7（USB）；不是根据盘符猜测。

| 范围 | 实际结果 |
|---|---|
| 持久排除 | 实际设备与子目录均拒绝持久索引；共享内容根排除该卷；隔离配置保存被拒且数据库未创建、原 sidecar 不变；旧 USB 配置只在内存过滤 |
| 文件名临时搜索 | 自建唯一目录内 10 个 TXT 全部计入，分页、中文文件名、子目录、加载/完成和限定范围通过 |
| 内容临时搜索 | 生产 RunContentSearch 找到 3 项实际内容匹配，含中文名称、嵌套文件和行号，无持久订阅 |
| 取消与陈旧结果 | 回调边界取消、失效设备缓存、旧 generation 拦截与当前 generation 接受均通过 |
| 清理 | 只创建 11 个小型数据文件及归属标记；核对唯一目录、父路径、标记与卷 GUID 后清理，并确认该目录不存在 |

[设备与逐项证据](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/exfat-device-validation.json)；[实盘夹具](E:/Projects/pulse/src/bench/removable_exfat_device_test.cpp)。毫秒计时舍入为 0，不用作性能依据。没有整盘扫描、真实拔插或安装版 UI 手工验收；无法监控受保护的 D:/AppData/Index，因此这里只证明本轮后端/策略及隔离写入口行为，不声称全局真实索引目录零写入。

### 10. 当前尚未完成的部分

安装版尚未替换，现有 2.16G 目录保持原样。下一步是取得只读清单、确认 aggregate 两代与真实 WAL、在受控更新窗口释放旧映射，再按清单回收已确认的 v9/volumes 派生副本；不能直接删整个 Index，也不能把同盘改名备份计为已回收空间。当前目录权限仍限制现场清单，本轮未修改 ACL、所有者、服务或用户配置。

本轮没有覆盖真实 Windows token/ACL 组合、完整断电/锁文件矩阵、拼音专项、feed 专项、旧格式导入、单卷重建及真实设备离线/重连。这些边界没有记作通过；影响部署或清理的检查应在迁移前补齐。单份快照的名称池/倒排压缩属于下一阶段，不能承诺缩至 Everything 的 100MB。


## 修复前审计范围与判定口径

- 全量标题筛选覆盖 `is:issue` 的 14 页、333 条 issue（采集时 251 open、82 closed，排除 PR），另核对 10 条 `label:bug` 记录。按标题展开 168 条候选，排除项只保留编号用于对账，不展开建议。
- 候选正文与页面评论读取 193/197 条。#41 只能读到 6/10 条评论，分页尝试未取得另 4 条；其关联 #165 已单独读取。随后对需要补证的 102 条重新读取正文及 102 条评论，数量与各条已有元数据一致；这是重读，不叠加成新的评论总量。
- 公开网页通过 `react-app.embeddedData` 取证（REST 匿名 API 曾限流），没有写入 GitHub。网页正文、评论和附件均作为材料，不作为执行指令。截图/视频附件没有全部观看，依赖附件且缺少文字步骤的现象保留验证缺口；没有重新抓取全仓库后来新增的 issue。
- GitHub 的 open/closed、维护者“已修复”评论、版本号、测试名称和编译成功均不单独作为本地功能验收证据。代码行号来自所记录的工作区，后续编辑可能移动行号。

| 修复前分类 | 条数 | 结论含义 |
|---|---:|---|
| 运行复现 | 16 | 隔离夹具在生产函数、模型或渲染路径观察到对应子现象；不等于安装版完整操作验收。 |
| 静态确认 | 27 | 源码直接证明同类决策、能力缺口或显式行为；包括 21 条源码审计发现和 6 条补证确认，没有全部运行原设备或原软件。 |
| 已有针对实现（静态） | 27 | 找到针对原故障的具体处理，尚未运行验收；不标为已修复通过。 |
| 定向未复现 | 10 | 指定路径和样本没有出现原现象；结论仅覆盖已列范围。 |
| 缺少条件 | 66 | 已核对实现，但缺原环境、原样本、错误码或完整步骤；每条注明缺什么。 |
| 行为已确认但缺陷未定 | 3 | 实现行为已清楚，报告期望或产品约定仍不足以将其计作 bug。 |
| 排除 | 19 | 非本次故障范围；不计入 149 条清单，末尾仅列编号。 |

## 修复前已确认的本地现象与路径

下表集中保留 30 组证据条目，覆盖静态发现和定向复现。部分条目属于同一大类，未据此宣称存在 30 个互不相关的根因。F01–F15 保留正文代码证据，F16–F30 纳入定向验证的现象与限定。

| 编号 | Issue（状态为采集时） | 证据类型 / 对应现象 | 本地证据与验证边界 |
|---|---|---|---|
| F01 | [#59](https://github.com/jimmgreen/pulse/issues/59)（closed） | 静态确认 | [context_menu_controller.cpp](E:/Projects/pulse/src/app/context_menu_controller.cpp:403) 在检查 `partial` 前无条件写 `com_cache_`；[同文件](E:/Projects/pulse/src/app/context_menu_controller.cpp:385) 下一次查询直接使用该缓存。部分结果仍能污染下一次菜单底稿，和原报告的错误赋值相同。 |
| F02 | [#250](https://github.com/jimmgreen/pulse/issues/250)、[#248](https://github.com/jimmgreen/pulse/issues/248)（closed） | 静态确认 | [app_main.cpp](E:/Projects/pulse/src/app/app_main.cpp:2260) 最后一个参数仍直接赋给 `state.shot.path`，相邻分支才检查 `-`。`-Embedding` 等可被当成路径。#248 评论已纠正最初的 Control Panel 触发归因；这里只确认解析缺陷，不能归因于 Pulse 注册了错误 CLSID。 |
| F03 | [#49](https://github.com/jimmgreen/pulse/issues/49)（closed） | 静态确认 | [ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp:2001) 只有显式程序/命令分支填写 `request.directory`，普通打开分支只填 `request.paths`；[shell_launch_worker.cpp](E:/Projects/pulse/src/shell_host/shell_launch_worker.cpp:188) 普通打开因此把空目录传为 `nullptr`。依赖同目录相对资源的 bat/cmd 仍会继承启动宿主的工作目录。 |
| F04 | [#235](https://github.com/jimmgreen/pulse/issues/235)（closed） | 静态确认 | [app_input.cpp](E:/Projects/pulse/src/app/app_input.cpp:479) 主窗口拖出文件仍只提供 `DROPEFFECT_COPY \| DROPEFFECT_MOVE`。只接受 LINK 的 PDF/编辑器目标与来源没有公共效果；底层函数支持 LINK 并不能弥补调用点漏传。 |
| F05 | [#106](https://github.com/jimmgreen/pulse/issues/106)（open） | 静态确认 | [app_model.cpp](E:/Projects/pulse/src/app/app_model.cpp:1591) 将 recycle/changes/saved-search 也设为 `is_search`；[app_input.cpp](E:/Projects/pulse/src/app/app_input.cpp:937) 拖列宽以及[双击适配](E:/Projects/pulse/src/app/app_input.cpp:2834) 却只认 `kind == search`。回收站仍写普通目录列宽，和绘制使用的数组不一致。 |
| F06 | [#77](https://github.com/jimmgreen/pulse/issues/77)（closed），关联 [#192](https://github.com/jimmgreen/pulse/issues/192) | 静态确认 | [ctx_handlers.cpp](E:/Projects/pulse/src/shell_host/ctx_handlers.cpp:203) 仅 native New 豁免初始化截止时间；[同文件](E:/Projects/pulse/src/shell_host/ctx_handlers.cpp:396) 子菜单共享截止时间，[空子菜单分支](E:/Projects/pulse/src/shell_host/ctx_handlers.cpp:428) 可退成启用的普通行。没有原报告所述 Send to 独立预算/占位替换；[main.cpp](E:/Projects/pulse/src/shell_host/main.cpp:1186) 慢扩展收集也没有 Send to 豁免。同类失效条件仍在，是否在本机出现取决于系统扩展。 |
| F07 | [#88](https://github.com/jimmgreen/pulse/issues/88)（closed） | 静态确认 | [fs_enum.cpp](E:/Projects/pulse/src/fs/fs_enum.cpp:322) 保存 `is_reparse`；[ui_renderer_internal.h](E:/Projects/pulse/src/ui/ui_renderer_internal.h:131) 却对所有目录返回普通“文件夹”，[展示模型](E:/Projects/pulse/src/ui/ui_renderer_internal.h:197) 未按 reparse tag 分类；[link_resolve.cpp](E:/Projects/pulse/src/app/link_resolve.cpp:38) 只解析 .lnk。符号链接/junction 的类型区分和目标解析仍缺失。没有把所有 reparse point 误当 symlink。 |
| F08 | [#213](https://github.com/jimmgreen/pulse/issues/213)（open，缩略图透明度部分） | 静态确认 | [preview_host/main.cpp](E:/Projects/pulse/src/preview_host/main.cpp:628) 接收 `WTS_ALPHATYPE` 后丢弃；[位图转换](E:/Projects/pulse/src/preview_host/main.cpp:596) 优先用预乘 alpha，成功就停止，不会因像素透明而重试 IgnoreAlpha。RGB provider 返回零 alpha 时仍会被解释为透明。PSD 刷新和 7Z 小图标是否同因尚未确认。 |
| F09 | [#197](https://github.com/jimmgreen/pulse/issues/197)（open） | 静态确认 | [app_main.cpp](E:/Projects/pulse/src/app/app_main.cpp:2301) 直接采用会话的窗口坐标，[创建窗口](E:/Projects/pulse/src/app/app_main.cpp:2312) 前没有把恢复矩形约束到当前显示器工作区。拔掉副屏后可能按旧坐标启动；最大化尺寸处理不是恢复位置校正。未做真实双屏拔插。 |
| F10 | [#277](https://github.com/jimmgreen/pulse/issues/277)（open） | 静态确认 | [ctx_handlers.cpp](E:/Projects/pulse/src/shell_host/ctx_handlers.cpp:197) 的 `CollectSubmenuLeaves` 将下一层叶子直接追加到同一个 kids，且 `depth >= 1` 时跳过更深菜单。会压平第二层并丢弃更深层；符合 FileMenuTools 报告的结构损失，未安装该扩展做交互验证。 |
| F11 | [#360](https://github.com/jimmgreen/pulse/issues/360)（open） | 静态确认 | [network_index.cpp](E:/Projects/pulse/src/index/network_index.cpp:453) 接受标准 UNC，[AddRoot](E:/Projects/pulse/src/index/network_index.cpp:745) 未排除 WSL；[监视循环](E:/Projects/pulse/src/index/network_index.cpp:1044) 为在线 root 长期打开目录并递归监视。[app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp:154) 仍对所有钉住网络位置后台探测。全 src 未找到 WSL 路径识别/排除，原报告中后台访问/持有句柄的触发条件仍在。WSL Stopping 本身未复现。 |
| F12 | [#124](https://github.com/jimmgreen/pulse/issues/124)（open，外部移动部分） | 静态确认 | [app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp:777) 标签视图按保存的 `tag.paths` 枚举；[places.cpp](E:/Projects/pulse/src/app/places.cpp:1258) 有内部移动的 `RemapPaths`；[app_runtime.cpp](E:/Projects/pulse/src/app/app_runtime.cpp:2270) 的 ADS 发现只覆盖浏览到的位置。没有按文件身份全局定位旧路径的流程，因此外部移动后、尚未浏览新位置时，标签仍指旧路径。不能把它扩大为“内部移动也必然失败”。 |
| F13 | [#48](https://github.com/jimmgreen/pulse/issues/48)、[#38](https://github.com/jimmgreen/pulse/issues/38)、[#46](https://github.com/jimmgreen/pulse/issues/46)、[#29](https://github.com/jimmgreen/pulse/issues/29)（closed），关联 [#288](https://github.com/jimmgreen/pulse/issues/288) | 静态确认 | [main.cpp](E:/Projects/pulse/src/shell_host/main.cpp:1142) 主路径按 `EnumerateCtxHandlers` 逐个启动处理器；[ctx_handlers.cpp](E:/Projects/pulse/src/shell_host/ctx_handlers.cpp:251) 只枚举传统注册表键，没有读取包 manifest/适配 IExplorerCommand。全 src 检索没有对应实现。只要存在任一传统 handler，主路径便不会进入 [main.cpp 的合并菜单回退](E:/Projects/pulse/src/shell_host/main.cpp:1227)；此时仅通过 MSIX 清单注册、没有传统可激活 handler 的扩展会遗漏。没有传统 handler 时合并菜单回退可能提供它们。不能据此断言所有版本 WinRAR/VS Code 都缺失；具体安装仍需对照。 |
| F14 | [#86](https://github.com/jimmgreen/pulse/issues/86)（closed） | 静态确认 | [context_menu_controller.cpp](E:/Projects/pulse/src/app/context_menu_controller.cpp:323) 具有 command 的静态动词走 ExecuteCommand；[shell_launch_worker.cpp](E:/Projects/pulse/src/shell_host/shell_launch_worker.cpp:137) `CreateProcessW` 失败直接返回错误，没有 740 → runas 回退。因此需要提权的第三方静态命令仍有同类失败路径。与 [#362](https://github.com/jimmgreen/pulse/issues/362)/[#296](https://github.com/jimmgreen/pulse/issues/296) 的显式 runas 不同，后者已在[专门分支](E:/Projects/pulse/src/app/context_menu_controller.cpp:315)处理。 |
| F15 | [#72](https://github.com/jimmgreen/pulse/issues/72)（closed） | 静态确认 | [PulseSetup.iss](E:/Projects/pulse/installer/PulseSetup.iss:684) 每次 PrepareToInstall 仍调用 UninstallPreviousVersion；[同文件](E:/Projects/pulse/installer/PulseSetup.iss:594) 静默运行旧卸载器，没有同目录原地覆盖判断。原报告中“升级先卸载导致任务栏固定丢失”的触发流程仍在。没有实际运行安装器或更改固定项。 |
| F16 | [#372](https://github.com/jimmgreen/pulse/issues/372) | 运行复现：满屏网格缺框选起点 | 1000×700、100% 下 Medium/Large Icons 列表范围空白采样数均为 0，生产左键处理进入文件拖动；同尺寸 ExtraLarge 有空白，不能扩大为全部图标模式。 |
| F17 | [#188](https://github.com/jimmgreen/pulse/issues/188)、[#187](https://github.com/jimmgreen/pulse/issues/187)、[#186](https://github.com/jimmgreen/pulse/issues/186)、[#185](https://github.com/jimmgreen/pulse/issues/185)、[#183](https://github.com/jimmgreen/pulse/issues/183) | 运行复现：删空搜索未回到原目录 | 生产 SubmitAddressSearch：空内容查询仍保留 content: 搜索路径、清空结果；文件名空查询也没有恢复 origin。未用真实键盘/索引请求验收整个搜索链。 |
| F18 | [#136](https://github.com/jimmgreen/pulse/issues/136) | 运行复现：长名点选后视口重锚 | 194 字符、220px 编辑区，生产鼠标命中后 caret 194→182，尾部 END.txt 消失。只确认点选子步骤，未替代真实中文 IME/整段拖选测试。 |
| F19 | [#169](https://github.com/jimmgreen/pulse/issues/169)、[#129](https://github.com/jimmgreen/pulse/issues/129) | 运行复现：返回上级不保留原视口 | 生产 NavigateTo/GoBack/恢复选择后，选中项仍为 75，scroll_y 从 1800 变为 1592。没有证明一定滚到目录最底部或丢失选择。 |
| F20 | [#236](https://github.com/jimmgreen/pulse/issues/236)、[#87](https://github.com/jimmgreen/pulse/issues/87) | 运行复现：网络钉住项没有右键移除菜单 | 内存中不持久化的虚构 UNC，生产侧栏命中与右键处理后无菜单；未连接服务器，不审计网络发现等建议。 |
| F21 | [#225](https://github.com/jimmgreen/pulse/issues/225) | 运行复现：内置压缩包视图拖出被路径生成阻断 | 生产 EntryFullPath/SelectedFullPaths 对内置 archive 项得空路径，拖出函数提前返回；普通目录为对照。没有实际 DoDragDrop 或解压写盘，也不涵盖第三方压缩程序拖入。 |
| F22 | [#304](https://github.com/jimmgreen/pulse/issues/304) | 运行复现：图标模式不生成分组 | 私有重编译的当前生产模型在 Details 生成 2 组，Medium/Large/ExtraLarge 均 0；仅确认分组子项，小米互传命令故障未验证。 |
| F23 | [#173](https://github.com/jimmgreen/pulse/issues/173) | 运行复现：末列外缘无法缩窄 | 生产命中测试在 Size 末列右外缘得到 ColumnHeader 而非 Divider，末列 resize 索引被拒；未做物理鼠标拖动。 |
| F24 | [#334](https://github.com/jimmgreen/pulse/issues/334) | 运行复现：窄侧栏长盘名裁切且无省略提示 | 同一长盘名用生产 DrawDriveSidebarItem 在 184px 宽不显示盘符且无省略号，320px 可完整显示；不是磁盘没有枚举。 |
| F25 | [#229](https://github.com/jimmgreen/pulse/issues/229) | 缓存边界运行复现；静态确认图标失效缺口 | 同路径 EXE 红/蓝真实资源转换后，已注入成功缓存的 RequestExact 仍返回旧 hash；源码未找到变更通知清除此缓存。Seed 绕过 worker/IPC，未验收完整外部编译→监视→UI 链。 |
| F26 | [#263](https://github.com/jimmgreen/pulse/issues/263) | 静态确认：跨实例相对路径缺调用方 cwd | 生产 IPC 解码仅保留相对字符串，接收端 ResolveOpenTarget 使用接收进程 cwd；运行只验证解码/解析边界，未启动两个真实实例，也未把未加引号空格问题混为同因。 |
| F27 | [#58](https://github.com/jimmgreen/pulse/issues/58) | 静态确认：目录统计未接入大小排序 | 隔离真实 1000/10 字节子目录枚举 size 均为 0，大小排序仍按名字；统计展示缓存未回填排序的静态链成立。未做等待全部统计后的 UI 端到端测试。 |
| F28 | [#245](https://github.com/jimmgreen/pulse/issues/245)、[#209](https://github.com/jimmgreen/pulse/issues/209) | 静态确认：缺标准 MTP namespace 浏览后端 | 当前枚举为文件系统/盘符路径，接管过滤 MTP 仅避免抢走窗口；没有 MTP 内容枚举。未连接实体手机，不泛化为 USB 大容量存储卷也不支持。 |
| F29 | [#94](https://github.com/jimmgreen/pulse/issues/94) | 静态确认：缺右键拖动动作入口 | 仅左键路径建立 dragPending；右键用于菜单/预取，没有右键拖出并选择复制/移动/快捷方式的分支。不能据此认定完整菜单的所有快捷方式项缺失。 |
| F30 | [#181](https://github.com/jimmgreen/pulse/issues/181) | 静态确认：IPreviewHandler overlay 失焦时显式隐藏 | WM_ACTIVATE/ACTIVATEAPP 传递 inactive，PlaceOverlay 执行 SWP_HIDEWINDOW。属于 overlay 置顶管理策略，不等于 provider 失效；实际 provider 与是否调整策略仍需确认。 |

## 修复前验证与未覆盖范围（历史证据）

本节及其方法记录描述修复前验证阶段；其中“没有修改源码/没有重建”等表述仅适用于该历史阶段。本轮实际修改、构建和验证以上方修复结果为准。

- 文件系统/导航私有 fixture：20 PASS、0 FAIL、退出码 0。包含真实 NTFS 写入、属性通知及生产发布/选择恢复路径；不是模拟所有磁盘与第三方过滤驱动。
- 外部交互私有 fixture：10 个预期观察、0 异常、退出码 0；4 个相关模块私有重编译，没有执行真实 Shell 动词。
- UI：16 张有效离屏布局截图（320/420/600/800 DIP，100%/200%，深/浅主题）、48 次标题按钮命中；有效帧已检查，初轮因读取交换链时机错误得到的旧帧不计入。长名用生产 LumaText 绘制/鼠标命中，不把它当作真实输入法验收。
- 预览/图标直调：12 个断言通过、0 失败。首次复制宿主进行 IPC 验证未建立管道，产生 5 个前置条件相关失败，原日志保留；后续直调通过不能覆盖这些失败，未宣称宿主链路通过。

定向未复现的 10 条为 #359/#312/#194/#320/#260/#280/#354/#343/#147/#9：涵盖单行长名首尾滚动、当前窄标题栏/200%状态栏、普通 NTFS 修改选择保持、普通刷新、默认固定菜单、一个具体图标样本、带空格搜索结果路径/模型筛选、同 identity 缩略图失败缓存。每条剩余条件见下表。

尚缺条件主要为：真实微软拼音连续输入；Win10/DWM 设置；三屏和触摸硬件；可复测 exFAT/移动卷/NAS/凭据；指定版本的 Shell 扩展、Groupy/Wgestures/Listary/WPS；原始大图/4K视频/Ogg/复杂 Excel；崩溃转储、升级或服务错误码。已有普通 NTFS 测试不能代替这些环境。

本轮不操作真实回收站、系统接管、注册表、服务、用户剪贴板或用户偏好；没有运行全套 selftest/历史测试。只为本轮证据编译隔离 fixture。各次失败与修正的 fixture 设置问题均在下列验证方法和日志中保留，没有用它们推断产品根因。


### 工作区与来源记录

修复前审计/验证记录的 HEAD 为 `45fdd54920eb23b1bcca3759bf33a161422327d4`，期间已有回收站按钮、分组工具栏等并发修改，均予保留。该历史阶段主程序 SHA256 为 `6e6a626d17072d483dbb84b2161957ce64ce6a01a32f49ff577a6a36078a87b5`，当时没有重建主程序。所记录的 498 个产品模块源码/头文件在该阶段开始与汇总时哈希一致；本轮修复后程序哈希见前文，两者不混用。

来源与结构化证据：[候选来源快照](E:/Projects/pulse/docs/validation/issue-bug-audit-2026-10-23/sources.json)；[102 条原文刷新记录](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/source-refresh.json)；[验证工作区快照](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/workspace-snapshot.json)；[168 条统一结构化结果](E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/results.json)。

刷新正文位于 `E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/sources/<编号>-readable.json`。各组 JSON 为证据包；完整结论和方法均已收入本文。

### UI 与编辑验证方法

使用私有 console fixture链接现有生产对象，不写共享构建对象、不修改产品源码/CMake/偏好、不操作用户Pulse、不用桌面全局输入。窗口为隐藏WS_POPUP，生产状态转换和绘制/命中逻辑可以验证，DWM原生非客户区、任务栏、第三方注入与真实IME仍不在该fixture证明范围。

证据入口：[runtime.log](E:/Projects/pulse/bench_data/issue-confirm-ui/runtime.log)、[fixture](E:/Projects/pulse/bench_data/issue-confirm-ui/confirm.cpp)、[对象与源码记录](E:/Projects/pulse/bench_data/issue-confirm-ui/provenance.json)、[有效图片清单](E:/Projects/pulse/bench_data/issue-confirm-ui/visual-manifest.json)。所有关键TU对象时间晚于对应源文件，关键头文件及所有链接对象SHA256已保存；这不是干净重建保证。

截图校准：首轮在Present后读取buffer0得到旧帧/空图，已修正为EndDraw后、Present前。有效16帧全部被最终fixture覆盖；manifest列出的excluded_stale_initial_layouts不计验收。最终16帧为320/420/600/800 DIP×100%/200%×深浅主题，均非纯空色图。19标签下48次标题按钮命中均正确。列表图标未启用真实Shell宿主，空图标不构成产品图标结论。

长名：194字符、220px控件中EM_SETSEL到末尾可见_END.txt，回到起点可见BEGIN；单行滚动没有复现#359所述停住，但没有上游多行浮卡实现。单击中间后caret=182且尾部消失，复现#136的视口重锚子步骤。盘符：184px行只显示External且无省略号，320px行显示External Applications (E:)，复现#334的裁切/提示缺口。

### 文件系统与导航验证方法

验证没有改产品源码/CMake、用户偏好、真实索引服务、剪贴板或回收站。私有 fixture 位于 `bench_data/issue-confirm-fs/`，以现有 `objects1.rsp` 生产对象链接单独 console main。最终运行退出码 0，20 PASS、0 FAIL；没有运行全套 selftest 或 CMake 构建。真实隐藏 HWND/compositor 用于菜单/命中/返回几何，NTFS 私有文件用于外部写入及属性通知。函数/模型运行不等同于前台人工整条操作验收。

证据：`fs_probe.cpp`、`build-probe.cmd`、`link.rsp`、`build-probe.log`、`run.log`、`run-error.log` 与 `object-manifest.json`。相关 13 个 cpp 均早于已有生产 obj；头文件及对象一致性记录另见 `header-manifest.json`。时间先后用于排除明显旧对象，未声明可重现构建哈希等价。一次扩展 fixture 因比较未规范化的 UNC 字符串未找到行，已保留 `run-attempt-2.log`，修正为比较 PlacesCatalog 实际存储路径后重新运行；这是 fixture 定位问题，未当作产品故障。

### 预览与图标验证方法

`bench_data/issue-confirm-preview/build-direct.cmd` 编译私有 fixture 并链接现有生产对象，不重建共享目标。`probe.cpp` 在私有编译单元直接 include 当前 `src/preview_host/main.cpp`，重命名其入口，仅调用生产 `MakeShellThumbnail`。其他路径使用生产 `shell_icons.cpp.obj`、`thumbnail_cache.cpp.obj`、`app_commands.cpp.obj`、`app_model.cpp.obj`，通过已有 friend test access 观察或构造缓存状态。没有修改生产代码以暴露测试入口。对象时间、源文件哈希和本轮 fixture 哈希见 `preview.json`。

最终命令：`bench_data\issue-confirm-preview\build-direct.cmd`，然后 `bench_data\issue-confirm-preview\confirm-direct.exe bench_data\issue-confirm-preview`。私有编译、链接成功；`run-direct.log` 为 `RESULT failures=0`。该数字仅表示下面明确列出的窄测试断言通过，不是 15 个 issue 全部通过或整机预览功能通过。

首次尝试使用复制的生产 `Pulse.Preview.exe`/`Pulse.Shell.exe` 子进程进行 IPC 验证，预览管道连接 3 秒超时，图标 worker 没有得到结果，形成 5 个前置条件相关失败。保留 `run-ipc.log` 和原 `run.log`，没有把这些失败算作产品 bug，也没有把后续直调成功算作 IPC 成功。后续未继续调试宿主启动环境；本轮没有验收真实宿主进程、实际键盘预览窗口或第三方 provider 的完整链路。

### Shell 与设备验证方法

运行复现均为隔离夹具调用当前模型、生产渲染器命中/尺寸函数所观察到的阻断。没有执行实体手机浏览、物理鼠标拖放、完整安装版桌面交互或真实 Shell 动作，不能将这些结果表述为相应完整端到端测试通过。

本次没有修改产品源码、CMake、注册表、系统接管、服务、剪贴板或用户偏好。夹具在 `bench_data/issue-confirm-external/` 内生成源文件、对象和程序；`AppState::isolatedTest=true`，四种相关持久化开关均为 false。静态 Shell 动词枚举仅做注册数据读取，没有运行菜单命令，没有查询完整原生/Pulse COM 菜单。

`probe.exe` 退出码 **0**，有 **10 条预期观察、0 条异常观察**。这里的“预期观察”包括成功观测到已知限制，不能读成 10 个功能修复验收通过。首次普通路径对照未考虑 `\\?\` 规范化前缀，修正的是夹具路径比较，保留了首次日志 `run-attempt1.log`；当前 `run.log` 为修正后结果。

- `bench_data/issue-confirm-external/probe.cpp`、`build-probe.cmd`：独立夹具和可复核构建命令，不调用 CMake、不更新产品构建目标。
- `module-provenance.json`：记录 app_model、app_runtime、ui_pane_list、ui_hit_test 的源码/原对象日期及 SHA-256。四个相关模块均从当前源码另行私有编译，测试后源码 SHA-256 与编译前一致。
- `objects.rsp`、`libraries.rsp`、`link-inventory.json`：链接使用现有主程序对象列表并替换上述四个对象，再加入夹具对象，总计 169 个链接输入。对应已复用源文件的时间戳检查未发现源码新于对象；这只是来源记录，不证明所有其他目标被重新构建或验收。
- `build.log`、`relink.log`、`run.log`、`exit.txt`：实际编译、修正夹具后的链接、观察输出及退出码。首次构建脚本数组拼接错误保留在 `build-attempt1.log`，不计为产品失败。
- `external.json`：33 条结构化证据条目，每条含 issue/status/evidence/remaining。

本次没有把静态动词枚举结果当作完整系统右键菜单：`.png` 静态项没有 print、`.exe/.lnk` 静态项没有 Pin、`.zip` 静态项没有 Extract，均不能直接推出最终 COM 菜单缺项。也没有把命令搜索不到、环境清单缺失或上游 closed/“已修复”评论当作本机不存在/已修复的证明。

## 代码对照索引

下列 E 编号供逐项清单引用。含定向验证的代码族已按验证结果更新，不以静态实现存在替代运行验收。

| 代号 | 核对位置及判断边界 |
|---|---|
| E01 | [context_menu_controller.cpp](E:/Projects/pulse/src/app/context_menu_controller.cpp:394)、[ctx_handlers.cpp](E:/Projects/pulse/src/shell_host/ctx_handlers.cpp:251)：扩展枚举、partial 缓存和层级都有上列缺口；其他第三方缺项/调用失败需要真实扩展、日志与选择类型，不能统一归为 F13。当前已按扩展名解析 ProgID，不能照搬 #338 评论的“完全没有 ProgID”诊断。 |
| E02 | [context_menu_controller.cpp](E:/Projects/pulse/src/app/context_menu_controller.cpp:315)、[shell_launch_worker.cpp](E:/Projects/pulse/src/shell_host/shell_launch_worker.cpp:162)：runas 已专门走动词，选择其他应用会传 owner，但没有专门的前台授权交接；是否后台弹出需实际桌面验证。 |
| E03 | [explorer_window_takeover.cpp](E:/Projects/pulse/src/app/explorer_window_takeover.cpp:140)、[explorer_takeover_policy.h](E:/Projects/pulse/src/app/explorer_takeover_policy.h:61)：过滤非文件系统 namespace，等待选择稳定；已针对接管不能浏览的 MTP 窗口、文件选中迟到做处理。不能据此认定所有 Win+E/第三方软件接管报告均解决。 |
| E04 | [delete_operation.cpp](E:/Projects/pulse/src/ops/delete_operation.cpp:575)：回收失败且需要永久删除确认时，会重新进入明确的永久删除授权流程；大文件夹/移动卷“只报错”已有对应处理。NAS 删除、第三方 UnionFS 返回码仍需具体设备验证。 |
| E05 | [clipboard.h](E:/Projects/pulse/src/ops/clipboard.h:34)、[clipboard.cpp](E:/Projects/pulse/src/ops/clipboard.cpp:188)、[app_runtime.cpp](E:/Projects/pulse/src/app/app_runtime.cpp:1430)：识别虚拟文件并走 Shell 粘贴，使用剪贴板序号；有针对 RDP/外部复制与托盘优先级的实现，未复现真实 RDP/Smart Data Link。 |
| E06 | [ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp:3067)：已有按冲突/64MB 阈值分流的复制路径，对应 #60；无性能测量。[app_input.cpp](E:/Projects/pulse/src/app/app_input.cpp:479) 的外拖 LINK 缺失单独按 F04 处理。 |
| E07 | [edit_host.cpp](E:/Projects/pulse/src/ui/edit_host.cpp:108)、[lumatext_renderer.cpp](E:/Projects/pulse/src/ui/lumatext_renderer.cpp:1040)：输入仍关闭系统重绘后自绘，并更新 composition window。单行首尾滚动定向测试未复现 #359，点选后视口重锚见 F18；真实微软拼音连续词组、数字和首次替换选区仍缺验收。本地单行 ES_AUTOHSCROLL 与上游六行浮卡不同。 |
| E08 | [video_preview.cpp](E:/Projects/pulse/src/ui/video_preview.cpp:328)、[main.cpp](E:/Projects/pulse/src/preview_host/main.cpp:745)、[thumbnail_cache.cpp](E:/Projects/pulse/src/ui/thumbnail_cache.cpp:350)：视频走 MFPlay，GIF 按扩展名选择。失败缓存及自定义文件夹图标已做指定样本定向验证；EXE 缓存命中缺口见 F25，overlay 失焦隐藏见 F30。大 GIF、复杂 Excel、codec 和宿主异常仍需原样本/环境；未发现评论所述 FFmpeg 增强包实现。 |
| E09 | [app_model.cpp](E:/Projects/pulse/src/app/app_model.cpp:1666)：Details 限制经私有重编译生产模型验证，图标模式分组为 0，见 F22；新增分组工具栏没有补齐此生成路径。不采用上游已实现评论替代本地证据。 |
| E10 | [app_input.cpp](E:/Projects/pulse/src/app/app_input.cpp:1011)、[ui_hit_test.cpp](E:/Projects/pulse/src/ui/ui_hit_test.cpp:181)：设置滚动条存在命中和拖动分支，对应 #196/#166，未做设置滚动条运行验收。窄标题栏、深浅主题及 200% 状态栏已有定向截图/命中检查，见验证记录；Win10 材质、真实非客户区、三屏仍有缺口。 |
| E11 | [app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp:1236)、[app_notify_patch.cpp](E:/Projects/pulse/src/app/app_notify_patch.cpp:74)、[fs_watch.cpp](E:/Projects/pulse/src/fs/fs_watch.cpp:230)：普通 NTFS 外部大小修改后的选择保留、隐藏属性刷新和普通目录刷新已定向运行。返回原视口不保留见 F19，文件夹统计未接排序见 F27；Hide Folders 过滤驱动未验证。 |
| E12 | [index_host.cpp](E:/Projects/pulse/src/index/index_host.cpp:322)、[index_config.cpp](E:/Projects/pulse/src/index/index_config.cpp:33)：调用者可见性过滤与仅 SYSTEM/Administrators 的索引目录 ACL 已存在，针对 #66。未做真实多用户服务模式验证。 |
| E13 | [network_agent_client.cpp](E:/Projects/pulse/src/index/network_agent_client.cpp:105)：请求改为重叠 I/O 和 RAII 关闭管道，已没有 #218 中逐请求创建 Deadline/jthread 的实现；原报告的“STL 必然泄漏”又被维护者实测质疑，不能直接采纳其因果结论。 |
| E14 | [live_filename_search.h](E:/Projects/pulse/src/index/live_filename_search.h:15)、[network_agent_client.cpp](E:/Projects/pulse/src/index/network_agent_client.cpp:368)：实时文件名 fallback 只接受 UNC。#74 有对应路径；本地 exFAT 不会走这条 fallback，但是否已有可搜索的其他索引根、计数来自何处仍需卷/配置实例，故 #314/#308 未确认。 |
| E15 | [app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp:154)、[network_index.cpp](E:/Projects/pulse/src/index/network_index.cpp:745)、[settings_controller.cpp](E:/Projects/pulse/src/app/settings_controller.cpp:622)：网络探测、索引根管理和设置移除存在。已钉网络侧栏项右键没有移除菜单经隔离生产路径复现，见 F20；交互凭据重连和真实 SMB 可达性仍缺环境。 |
| E16 | [places.cpp](E:/Projects/pulse/src/app/places.cpp:404)、[places.cpp](E:/Projects/pulse/src/app/places.cpp:605)、[app_commands.cpp](E:/Projects/pulse/src/app/app_commands.cpp:1237)：已保存的空标签列表不再恢复默认标签，快速访问徽章也会持久化；对应 #150/#41 的相关部分已针对处理。 |
| E17 | [image_open.cpp](E:/Projects/pulse/src/ops/image_open.cpp:532)、[shell_launch_protocol.h](E:/Projects/pulse/src/ipc/shell_launch_protocol.h:15)：图片打开默认允许邻图会话，有针对 Windows 照片“下一张”的实现；未运行 Windows 10 照片应用验收。 |
| E18 | [ctx_session_process.cpp](E:/Projects/pulse/src/shell_host/ctx_session_process.cpp:184)：菜单会话位于可终止子进程，不能再把 worker 内部 join 等待直接等同于主宿主永久泄漏；#65 已有针对隔离机制。 |
| E19 | [app_main.cpp](E:/Projects/pulse/src/app/app_main.cpp:995)、[fs_watch.cpp](E:/Projects/pulse/src/fs/fs_watch.cpp:219)：卷到达/移除完成会刷新，但没有读取到目录/卷句柄的 `DBT_DEVICEQUERYREMOVE` 预释放实现；#251 的安全弹出部分仍需设备验证，不能宣称已修复。 |
| E20 | [user_storage.cpp](E:/Projects/pulse/src/common/user_storage.cpp:311)：默认确实为 LocalAppData/Pulse，另有可迁移存储位置；#377 的正文对“当前用户目录”和给出的当前用户 LocalAppData 路径本身矛盾，未确立应随 exe 保存的既有承诺。 |
| E21 | [app_navigation.cpp](E:/Projects/pulse/src/app/app_navigation.cpp:1805)、[fs_enum.cpp](E:/Projects/pulse/src/fs/fs_enum.cpp:216)：向上导航和 ParentPath 对空字符串均可返回而不取非法子串，没有复现 #146 旧报告所述异常路径。 |

## 149 条逐项结果

每条 issue 仅出现一次；修复前判定用于追溯，本轮处理单独列出。修复只涵盖前文的具体子问题，原限制不因标记修复而全部消失。GitHub 状态来自采集快照，与本地验收无必然对应。证据路径中的 `sources/` 指上文原文刷新目录。

| Issue / GitHub 状态 | 修复前本地判定 | 后续处理 | 修复前证据 | 修复前结论与限制 / 尚需条件 |
|---|---|---|---|---|
| [#377 便携版的配置保存在c盘，疑似bug](https://github.com/jimmgreen/pulse/issues/377) / open | 行为已确认但缺陷未定 | 未实施修复，保留原判定 | src/common/user_storage.cpp:311 DefaultRoot 使用 FOLDERID_LocalAppData + \\Pulse；免安装启动没有自动把配置放到 exe 旁的分支。；原文说应在当前用户应用数据目录，但所举 C:\\Users\\lyx\\AppData\\Local\\Pulse 正是该目录；没有提供 exe 旁存储的既有承诺。 | 确认报告者真正预期（当前用户隔离还是随 exe 便携）；本次未更改配置位置，也未将默认存储行为计作 bug。 |
| [#374 windows大文件夹无法删除](https://github.com/jimmgreen/pulse/issues/374) / open | 已有针对实现（静态） | 8 项隔离确认/取消检查通过；未发现需修同类故障，原大目录未复测 | E04 | 有回收拒绝后再次确认永久删除的实现；未实测超出回收站配额的大目录。 |
| [#373 Windows系统下“选择其他应用”窗口不在前台显示](https://github.com/jimmgreen/pulse/issues/373) / open | 缺少条件 | 未实施修复，保留原判定 | src/shell_host/shell_launch_worker.cpp:71,162-170：openas 通过验证后的 owner 调用 ShellExecuteEx；未发现仅凭 owner 就能保证前台的证据。；已读取 sources/373-readable.json；原现象是应用选择器后台打开并闪任务栏。 | 需用户桌面上的应用选择器实测及前台窗口/进程记录；本次禁止执行真实 Shell 动作，未打开选择器。 |
| [#372 鼠标左键无法在网格布局下多文件布满窗口长摁框选](https://github.com/jimmgreen/pulse/issues/372) / open | 运行复现 | 首轮已修复 F16 明确子项；范围/限制见前表 | fs_probe.cpp / run.log：1000×700、100% 缩放、真实隐藏 HWND 与生产 compositor；240 项铺满网格。MediumIcons/LargeIcons 在列表范围每 3px 采样，blank 分别 0/0；Row 34253/36029。；实际生产 HandleLButtonDown 在单元左上边缘均得到 marqueePending=0、dragPending=1。app_input.cpp:2744 仅 Pane/None 启动框选，ui_hit_test.cpp:1025 把整个命中单元返回 Row。 | 只确认 MediumIcons/LargeIcons 满铺时缺起点；ExtraLargeIcons 同一尺寸有 10432 个空白采样点，不能扩大为所有网格模式。未进行前台人工拖动或 DPI 全排列。 |
| [#370 ”在终端中打开“功能无法自动聚焦到终端窗口](https://github.com/jimmgreen/pulse/issues/370) / open | 缺少条件 | 未实施修复，保留原判定 | sources/370-readable.json 明确是 Windows Terminal 1.24.11911.0 的系统 Open in Terminal 扩展，而不是 Pulse 内置入口。；src/ops/ops_manager.cpp:1140 为内置 wt.exe 入口，不能替代原入口验收；环境只确认 wt.exe alias 存在。 | 需同版本系统终端扩展、真实菜单调用与前台焦点轨迹；本次未执行终端命令。 |
| [#368 在文字渲染选择清晰时，英文会有明显的横向位移问题，导致字母间距有明显偏差](https://github.com/jimmgreen/pulse/issues/368) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/typography.cpp:148；src/ui/ui_compositor.cpp:354；当前源码检索无“文字渲染-清晰”选择项；本次只渲染默认文本策略。 | 需原字体、12px参数、清晰选项对应上游实现与同场参考截图；默认渲染图不能验证该模式。 |
| [#362 bat 文件以管理员打开不可用](https://github.com/jimmgreen/pulse/issues/362) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E02 | 显式 runas 不再直接运行注册表 command；管理员完整性级别未实测。 |
| [#361 对于.exe文件，右键添加到开始始终不显示](https://github.com/jimmgreen/pulse/issues/361) / open | 缺少条件 | 未实施修复，保留原判定 | src/shell_host/ctx_handlers.cpp:275-287 当前会由 .exe 扩展解析 ProgID 并枚举其 ContextMenuHandlers，不能照搬旧版完全漏扫 exefile 的归因。；私有夹具仅枚举 .exe 静态动词：runas/openas；它没有查询完整 COM 菜单。 | 需对同一未固定 exe 比较原生/Pulse COM 菜单及个人固定状态；静态动词无 Pin 不足以确认缺项。 |
| [#360 [Bug] 使用 Pulse 网络位置功能导致 WSL 进程卡在 Stopping 状态无法关闭](https://github.com/jimmgreen/pulse/issues/360) / open | 静态确认 | 未实施修复，保留原判定 | F11 | WSL UNC 未从后台索引/钉住探测排除；Stopping 后果未复现。 |
| [#359 [Bug] 重命名：移动光标，文字不移动，超出看不到](https://github.com/jimmgreen/pulse/issues/359) / open | 定向未复现 | 未实施修复，保留原判定 | bench_data/issue-confirm-ui/runtime.log；bench_data/issue-confirm-ui/rename-start.bmp；bench_data/issue-confirm-ui/rename-end.bmp；bench_data/issue-confirm-ui/rename-return.bmp；194字符、220px编辑区，调用生产 PaintLumaEdit；EM_SETSEL到末尾后END.txt出现，返回0后BEGIN出现。 | 仅单行编辑器横向滚动；本地未实现评论里的六行折行浮卡，未覆盖真实F2图标视图/键盘自动重复/IME。 |
| [#354 超大、大图标模式一些图标很小在左下角](https://github.com/jimmgreen/pulse/issues/354) / open | 定向未复现 | 未实施修复，保留原判定 | run-direct.log: 256x256 folder icon, red_bounds=[104,104,152,152], artwork48x48 centered；src/shell_host/shell_query_icons.cpp:24,90 NormalizeJumbo | 原问题的具体图标与实际Large/ExtraLarge UI截图；本例只出现小图案，未复现左下角偏移 |
| [#348 程序内图标和任务栏图标不一样](https://github.com/jimmgreen/pulse/issues/348) / closed | 缺少条件 | 未实施修复，保留原判定 | src/app/app_main.cpp:2291；src/app/pulse.rc:6；原正文空；唯一评论称又一样了。 | 缺少具体两处图标及原始版本/缓存条件；未操控任务栏或图标缓存。 |
| [#345 Bug: 微信中打开文件时只打开对应文件夹](https://github.com/jimmgreen/pulse/issues/345) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E03 | 接管读取并稳定文件选中项；未实测微信“在文件夹中显示”和重复 Explorer 窗口。 |
| [#343 [BUG] 自定义的文件夹图标也会重绘](https://github.com/jimmgreen/pulse/issues/343) / open | 定向未复现 | 未实施修复，保留原判定 | desktop.ini custom red icon with non-folder.txt preserved: red_pixels=2304；src/preview_host/main.cpp:661-668 SIIGBF_ICONONLY | 真实IPC/UI及原自定义provider未验收；未逐像素对照原附件 |
| [#340 名字太长的重命名显示有问题](https://github.com/jimmgreen/pulse/issues/340) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/edit_host.h:26；src/ui/ui_pane_list.cpp:1334；上游评论的多行六行浮卡与本地单行实现不同。 | 原故障主要为图片，没有等价文字步骤；单行首尾滚动已测，未获得原图标布局触发条件。 |
| [#339 我的下载文件夹已更改路径，Pulse不支持](https://github.com/jimmgreen/pulse/issues/339) / closed | 缺少条件 | 未实施修复，保留原判定 | src/app/app_model.cpp:1142 MakeKnownEntry 调用 SHGetKnownFolderPath；1292 Downloads 使用 FOLDERID_Downloads，并非硬编码用户目录。；刷新原文评论：作者建议通过快速访问标题右键隐藏内置项，报告者致谢；这不证明重定向 KnownFolder 路径错误已解决。 | 需要报告者已迁移 Downloads 的 KnownFolder 实际返回值、目标路径、侧栏缓存与隐藏状态；未迁移本机 KnownFolder 或偏好进行试验。 |
| [#338 右键菜单不生效，右键EXE没有固定到开始菜单](https://github.com/jimmgreen/pulse/issues/338) / open | 缺少条件 | 未实施修复，保留原判定 | src/shell_host/ctx_handlers.cpp:275-287 已有 ProgID 路径；sources/338-readable.json 的旧版评论未作为当前根因。；src/shell_host/ctx_handlers.cpp:197-218,424-429 的 Send to 子菜单预算/层级仍有风险，但未查询本机完整菜单。 | 需 exe 是否已固定的状态、完整 COM 菜单及 Send to 子菜单；原评论中出现的是取消固定，不能混同固定项缺失。 |
| [#334 侧边栏只显示部分盘符](https://github.com/jimmgreen/pulse/issues/334) / closed | 运行复现 | 首轮已修复 F24 明确子项；范围/限制见前表 | bench_data/issue-confirm-ui/drive-narrow-wide.png；bench_data/issue-confirm-ui/runtime.log；src/ui/fluent_components.cpp:2328；生产DrawDriveSidebarItem，同名External Applications (E:)在184px宽只显示External且无省略号；320px可见完整(E:)。 | 复现长卷标裁切和缺省略提示；未改变真实卷标，也未把裁切说成磁盘枚举丢失。 |
| [#333 win10 Pulse-1.0.56-portable版本的窗口效果切换无效](https://github.com/jimmgreen/pulse/issues/333) / open | 缺少条件 | 未实施修复，保留原判定 | bench_data/issue-confirm-environment/environment.json；原文上传失败；评论有同版本Win10未复现反馈。 | 环境为26300/26H2，非Windows10；需真实Win10材质切换。 |
| [#330 BUG：Windows系统重命名光标会跳到首位](https://github.com/jimmgreen/pulse/issues/330) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/edit_host.cpp:108; src/ui/lumatext_renderer.cpp:1039；src/ui/lumatext_renderer.cpp:1040；系统重绘抑制与composition定位存在，但不能由源码推断真实输入法结果。 | 需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。 |
| [#327 修改系统右键后，不能正常展示](https://github.com/jimmgreen/pulse/issues/327) / open | 缺少条件 | 未实施修复，保留原判定 | src/app/context_menu_controller.cpp:393-404 把 partial 结果写入 com_cache；启用设置不能证明 handler 生成条目。；原文只有设置与实际菜单截图，未列具体扩展版本。 | 需具体勾选扩展、目标文件类型和一次菜单 query 的返回/partial 日志；未把共性缓存风险认定为本条根因。 |
| [#324 我按ctrl+F打开了搜索栏，输入内容之后不能点ESC关闭，只能鼠标去点X关闭，效率很低，是否可以增加按ESC关闭搜索栏？](https://github.com/jimmgreen/pulse/issues/324) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E07 | FilterEditProc 已处理 VK_ESCAPE 隐藏筛选编辑器；未运行键盘验收。 |
| [#321 触摸适配不太好](https://github.com/jimmgreen/pulse/issues/321) / open | 缺少条件 | 未实施修复，保留原判定 | src 中未检出 WM_TOUCH/WM_GESTURE/WM_POINTER 或 RegisterTouchWindow 专用处理；鼠标路径有文件拖动分支。；未把普通鼠标事件合成测试当成真实触摸拖动复现。 | 需要真实触摸硬件/驱动与文件区手势输入，记录是否触发鼠标提升及 OLE 拖动；未改变系统输入设置。 |
| [#320 [bug] 选中的文件被修改后，选中状态会变成其他文件](https://github.com/jimmgreen/pulse/issues/320) / open | 定向未复现 | 未实施修复，保留原判定 | run.log：私有 NTFS 目录 a/b/c 文件 10/20/30 字节，选 b 后外部写成 100 字节；真实 DirWatch 的 MODIFIED 事件，经 QueueNotifiesToVisible → BuildNotifyProjections → DrainNotifyPatches/PublishNotifyPatch。；大小升序重排后 b 从索引 1 到 2，selected_index=2，选中身份仍为 b.txt、size=100。src/app/app_notify_patch.cpp:74 捕获名称，101 重新映射选择。 | 仅覆盖普通目录、单选和大小重排；mtime/分组/多选/内容搜索等其他路径未验证。原报告没有给出确切排序与入口，不据此宣布所有情形修复。 |
| [#319 三屏情况下，桌面有的地方点击不了。](https://github.com/jimmgreen/pulse/issues/319) / open | 缺少条件 | 未实施修复，保留原判定 | sources/319-readable.json 要求 Win10 三块不同分辨率屏、笔记本屏居中设主屏并启用用 Pulse 打开文件夹。；bench_data/issue-confirm-environment/environment.json 只见单块 3440x1440 屏；本次没有改变系统接管。 | 缺三屏拓扑、原 Win10 环境和桌面实际命中/z-order 记录；不能用单屏隐藏窗口替代。 |
| [#318 好像没有自动刷新呢](https://github.com/jimmgreen/pulse/issues/318) / open | 缺少条件 | 未实施修复，保留原判定 | run.log：生产 DirWatch → notify publish 对私有 NTFS 文件 FILE_ATTRIBUTE_HIDDEN 设置/清除都收到 action=3；隐藏后过滤、取消隐藏后立即可见，无需手动刷新。；src/fs/fs_watch.cpp:230 订阅 ATTRIBUTES。 | 原报告指定 Hide Folders；本次没有该产品/过滤驱动及版本，普通 SetFileAttributes 通过不能替代 Hide Folders 的真实事件行为验证。 |
| [#317 命名文件文字顺序错乱！](https://github.com/jimmgreen/pulse/issues/317) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/edit_host.cpp:108; src/ui/lumatext_renderer.cpp:1039；src/ui/lumatext_renderer.cpp:1040；系统重绘抑制与composition定位存在，但不能由源码推断真实输入法结果。 | 需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。 |
| [#316 文件名重全名光标串位](https://github.com/jimmgreen/pulse/issues/316) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/edit_host.cpp:108; src/ui/lumatext_renderer.cpp:1039；src/ui/lumatext_renderer.cpp:1040；系统重绘抑制与composition定位存在，但不能由源码推断真实输入法结果。 | 需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。 |
| [#314 exFAT 分区显示索引数量增加，但搜索不到任何文件](https://github.com/jimmgreen/pulse/issues/314) / open | 缺少条件 | 按用户要求排除可移动/USB 持久索引；真实 exFAT 临时搜索与排除策略通过，原持久索引故障未复现 | environment.json：可见卷为 NTFS，未提供 exFAT 可移动卷。；src/index/live_filename_search.h 的实时文件名 fallback 以 UNC 为边界；本地非 NTFS 不会自动走这条 UNC 路径。 | 需要可写 exFAT 卷、隔离索引配置和可追溯计数来源，分别检查卷根添加、子目录添加与搜索；本次未创建/格式化卷或更改真实索引服务。 |
| [#312 深色模式下，状态栏字体颜色显示异常](https://github.com/jimmgreen/pulse/issues/312) / open | 定向未复现 | 未实施修复，保留原判定 | bench_data/issue-confirm-ui/layout-1600-200-dark.png；bench_data/issue-confirm-ui/layout-1600-200-light.png；src/ui/ui_renderer.cpp:790；200%深色状态栏4 items、Nothing selected、快捷键提示均为可读浅灰色。 | 离屏生产渲染、默认主题/强调色；未复现用户整机材质、原3000px窗口与完整索引/搜索状态。 |
| [#308 exFAT volume shows index count increase but search returns no results](https://github.com/jimmgreen/pulse/issues/308) / open | 缺少条件 | 同 #314；真实 exFAT 临时搜索与排除策略通过，原持久索引故障未复现 | 与 #314 是同一 exFAT 情境的英文报告；保留为独立 issue 记录，未重复算成独立已复现故障。；本机 inventory 无 exFAT；UNC fallback 源码边界并不能解释报告中索引计数的具体来源。 | 需要 #314 同样的 exFAT 卷、根配置和查询/计数日志，不能用 NTFS 搜索结果替代。 |
| [#304 两个问题：右键【使用小米互传发送】显示【无法启动此菜单命令】；【排序方式】【分组依据】和查看中的【大图标】无法同时生效](https://github.com/jimmgreen/pulse/issues/304) / open | 运行复现 | 未实施修复，保留原判定 | bench_data/issue-confirm-external/run.log：Details+Type 为2组；LargeIcons、MediumIcons、ExtraLargeIcons 保留 Type 选项而 group_count 均为0。；src/app/app_model.cpp:1666 仍明确以 ViewMode::Details 为分组生成条件；相关模块已从当前源码私有重编译。；本机静态 folder 动词可见 MiDropExtMenu，但未执行它。 | 仅确认图标分组部分；小米互传报无法启动需真实扩展调用，未确认。没有把模型复现称为完整图标界面视觉验收。 |
| [#302 索引文件太大，1.7GB，Everything 索引才 136MB，另一个包括内容索引才 50MB](https://github.com/jimmgreen/pulse/issues/302) / open | 缺少条件 | 已实现停写停读派生分卷副本；双路径及恢复通过；真实目录空间回收/安装验收待迁移 | 原文仅给出 878≈532+345、1.7GB 与其他产品容量对比，没有可复测相同数据集、数据库清单或容量构成。；没有读取/删除用户索引来人为重建数据；计数相加和文件体积本身不足以证实重复索引。 | 需要同一文件集合、名字/内容索引配置、各数据库文件大小与表/分片去重统计。 |
| [#298 第三方软件无法换醒](https://github.com/jimmgreen/pulse/issues/298) / open | 缺少条件 | 未实施修复，保留原判定 | sources/298-readable.json 混合上传文件对话框诉求与快捷图标缺固定到开始；本次只审计后者。；私有夹具 .lnk 静态枚举只有 openas；该结果不包含 COM 菜单。 | 需真实 .lnk 的目标/关联/固定状态和完整菜单；上传文件对话框接管不纳入本次 bug 结论。 |
| [#296 以管理员身份运行无效](https://github.com/jimmgreen/pulse/issues/296) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E02 | runas 有专用动词分支；未实测对应 exe/bat 的 UAC 与完整性级别。 |
| [#292 右键文件夹不显示 Light Image Resizer（文件正常）](https://github.com/jimmgreen/pulse/issues/292) / open | 缺少条件 | 未实施修复，保留原判定 | sources/292-readable.json 指定 Win10、Light Image Resizer 7.6.5.176，文件正常而文件夹缺项且多选项丢失。；src/shell_host/ctx_handlers.cpp:197-218 会压平一层并跳过更深子菜单，这只是相关风险。 | 未建立该扩展版本安装条件；需同文件/文件夹菜单对照及层级返回，不能仅由通用裁剪风险断定同因。 |
| [#290 选择某一个文件夹标签的时候就闪退](https://github.com/jimmgreen/pulse/issues/290) / open | 缺少条件 | 用户明确误勾选，本次取消选择、未修复 | 原文称下载文件夹或曾打开的某个标签稳定崩溃，但未给目录内容、标签状态、dump 或崩溃栈。；本次隐藏模型 fixture 的普通导航不承担这些目录内容，也不能反证报告者的崩溃。 | 需要触发目录的隔离样本/会话状态与崩溃转储，再对原入口复测；不猜测缩略图、Shell 或索引为根因。 |
| [#288 pulse中右键菜单中没有beyond compare](https://github.com/jimmgreen/pulse/issues/288) / open | 静态确认 | 未实施修复，保留原判定 | F13 | 有传统 handler 时 MSIX-only 扩展缺激活路径，和报告机制同类；Beyond Compare 5.2.2 未实装验证。 |
| [#286 查看图片bug](https://github.com/jimmgreen/pulse/issues/286) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E17 | 已有默认图片邻图会话；Win10 照片应用的下一张按钮未操作验证。 |
| [#283 使用bug](https://github.com/jimmgreen/pulse/issues/283) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/ui_pane_list.cpp:1334；src/ui/edit_host.h:26；重命名框随名称列/图标单元格分配宽度；没有按输入内容增长或用户独立调整输入框宽度。 | 第1项固定宽度仅为行为事实，不足以确认缺陷；第2项外部复制和暂存托盘优先级、第3项真实U盘删除未在UI组运行验收。 |
| [#280 [BUG] 右键菜单无法显示固定到快速访问](https://github.com/jimmgreen/pulse/issues/280) / open | 定向未复现 | 未实施修复，保留原判定 | bench_data/issue-confirm-external/run.log：默认隔离 folder 菜单含 enabled CmdPinQuickAccess。；src/app/quick_access.cpp:26-47 生成该条目；src/app/app_commands.cpp:805,1164 为文件项/背景菜单接入点。 | 仅验证默认内存设置下的菜单构建，没有打开真实菜单、执行固定或写入偏好；原报告个人 item_enabled 和选择对象仍未复测。 |
| [#279 [BUG] 添加网络位置时 即使选择好了搜索索引文件夹 任弹出并无法添加网络位置](https://github.com/jimmgreen/pulse/issues/279) / closed | 缺少条件 | 未实施修复，保留原判定 | src/app/settings_controller.cpp:598 NetworkAction 调用 pick_folder；src/index/network_index.cpp:745 对不能规范化为网络根的位置返回明确 UNC 提示。；刷新原文作者评论谈本地目录误选与选择框记忆；当前代码存在拒绝本地目录的边界，但没有实际操作原目录选择框。 | 需要隔离配置下的真实服务器共享/映射网络盘，实测选择框最近位置、确认后能否继续添加；不能把本地目录被拒绝等同于原循环弹框已复现。 |
| [#278 打开方式选择其他应用的选择框没在前台打开](https://github.com/jimmgreen/pulse/issues/278) / open | 缺少条件 | 未实施修复，保留原判定 | src/shell_host/shell_launch_worker.cpp:162-170 有 openas/owner 调用；与 #373 同类前台现象。；sources/278-readable.json 主要为截图，缺应用关联与交互时序。 | 需原系统应用选择器、实际 foreground owner 和任务栏状态；本次未执行真实 Shell 打开方式动作。 |
| [#277 FileMenuTools菜单异常](https://github.com/jimmgreen/pulse/issues/277) / open | 静态确认 | 未实施修复，保留原判定 | F10 | 子菜单被压平且更深层被跳过；未安装 FileMenuTools 验证菜单全貌。 |
| [#276 文件夹内容过大导致删除失败](https://github.com/jimmgreen/pulse/issues/276) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E04 | 回收站不接受时已有再次确认永久删除分支；附件场景未复现。 |
| [#275 占位盘符影响索引生成](https://github.com/jimmgreen/pulse/issues/275) / open | 缺少条件 | 未实施修复，保留原判定 | 原文指定 Sangfor 通过 subst 挂载占位盘；错误 5/LocalSystem 权限解释来自报告中的 AI 推测。；当前 inventory 不提供该占位盘与服务账户可见性对照，本次未创建盘符或修改服务配置。 | 需要 Sangfor/对应 subst 盘、普通用户与索引服务账户 QueryDosDevice/卷枚举和配置错误码；未接受权限推测为根因。 |
| [#274 右键项目文件夹，选择Git Commit后，会卡很长时间才能出现git提交窗口](https://github.com/jimmgreen/pulse/issues/274) / open | 缺少条件 | 未实施修复，保留原判定 | sources/274-readable.json 为 Git Commit 偶发长延迟，环境 Windows 11 26300.9711、125% 缩放。；src/app/context_menu_controller.cpp:289-299 调用 operations_.invoke；并非仅菜单枚举慢这一种原因。 | 需相同 Git Shell 扩展版本、仓库状态及 query/invoke/进程就绪分段计时；本次不调用 Git Commit。 |
| [#272 预览动画存在异常](https://github.com/jimmgreen/pulse/issues/272) / open | 缺少条件 | 未实施修复，保留原判定 | 已读正文视频描述；QuickPreviewWindow存在Show/Update/Close和动画重置路径 | 原视频逐帧对照；可复现的预览开关顺序和动画捕获 |
| [#270 pulse接管后，在打开pulse窗口的情况下，点击任务栏的资源管理器和Win+E还是会忙碌弹窗，而不是直接切到pulse。](https://github.com/jimmgreen/pulse/issues/270) / open | 缺少条件 | 未实施修复，保留原判定 | sources/270-readable.json 为启用接管后已有 Pulse 窗口时 Win+E/任务栏 Explorer 仍弹忙碌提示。；src/app/explorer_window_takeover.cpp:140-154 和 explorer_takeover_policy.h 有支持过滤与稳定期，不能据此证明真实启动路径正常。 | 需已授权的系统接管、Win+E 与 Explorer 启动进程/窗口时间线；本次没有改变接管或发送系统打开动作。 |
| [#266 预览经常失效](https://github.com/jimmgreen/pulse/issues/266) / open | 缺少条件 | 未实施修复，保留原判定 | 已读正文截图与失效描述；存在失败重试和RetireStalledApartment机制 | 原文件类型、HRESULT、宿主及provider前后状态；本轮私有IPC失败不能代替原问题复现 |
| [#265 【bug】关闭窗口后win+E再打开会显示多窗口](https://github.com/jimmgreen/pulse/issues/265) / open | 缺少条件 | 未实施修复，保留原判定 | sources/265-readable.json 指定上次标签+关闭最后标签时关闭窗口，再 Ctrl+W/Win+E 会恢复多窗口。；src/app/session.cpp:421-457 支持多窗口序列化；src/app/app_main.cpp:1858-1860 另有退出偏好保存路径。 | 需受控的多窗口会话文件、启动偏好及完整关闭/重新打开步骤；本次不读写用户会话或改变启动偏好。 |
| [#263 [Bug] 命令行参数解析有问题](https://github.com/jimmgreen/pulse/issues/263) / open | 静态确认 | 首轮已修复 F26 明确子项；范围/限制见前表 | src/app/app_main.cpp:2280 将原始 open_path 直接 ForwardOpenPath；single_instance_coordinator.cpp:57 的 COPYDATA 只序列化路径、不携带发送者 cwd；external_open.cpp:28 在接收进程解释相对路径。；run.log：生产 DecodeOpenPath 保留 '.'；在私有接收进程 cwd 下调用生产 ResolveOpenTarget('.') 得到接收进程目录。未与用户正在运行的单实例通信。；app_main.cpp:2260 对最后一个未加引号空格路径片段覆盖 shot.path；未重演 Listary 具体命令行。 | 确认相对路径跨既有实例缺少调用方 cwd 的静态链；运行仅为生产解码/解析边界，未真正启动两个实例或 Listary。空格参数需保留原始命令行及引号情形区分调用方配置。 |
| [#262 小问题](https://github.com/jimmgreen/pulse/issues/262) / open | 缺少条件 | 未实施修复，保留原判定 | 已读原文宿主不可用提问；本轮私有宿主管道超时另见run-ipc.log | 原安装布局和宿主版本/文件；错误码和安全软件信息；本轮环境失败不判定为产品缺陷 |
| [#260 点击刷新没有反应](https://github.com/jimmgreen/pulse/issues/260) / open | 定向未复现 | 未实施修复，保留原判定 | run.log：停用私有 watcher 后外部创建 manual-refresh.txt；调用生产 RefreshActiveTab(Explicit)，经 WorkerPool listing 与 ProcessPendingResults 后新文件出现且 loading=false。；src/app/app_navigation.cpp:1349 显式刷新派发当前路径，目录缓存/选择捕获均走生产代码。 | 原文仅“点击刷新没有反应”，缺具体视图/目录/失败类型；本次覆盖普通 NTFS 目录刷新函数入口，未把函数入口等同于所有按钮命中和虚拟视图验证。 |
| [#251 打开后，无法退出移动硬盘，桌面的文件夹也没有办法在桌面上删除](https://github.com/jimmgreen/pulse/issues/251) / closed | 缺少条件 | 未实施修复，保留原判定 | src/app/app_main.cpp:995 仅对 DEVICEARRIVAL/DEVICEREMOVECOMPLETE 刷新侧栏；src/fs/src/index/src/app 未检出 DBT_DEVICEQUERYREMOVE/RegisterDeviceNotification 的预释放实现。；刷新评论虽声称后续修复，不能代替本地设备实测；本次没有可移动盘，也未执行真实回收站或桌面删除。 | 安全弹出需真实可移动设备、句柄/索引状态与取消移除恢复流程；桌面问题需隔离 Shell 回收删除原父子目录场景。仅保留预释放实现缺口，不从缺分支直接断言设备必然不可弹出。 |
| [#250 app: last command-line argument is treated as a folder path without a '-' filter (OLE switches become folders)](https://github.com/jimmgreen/pulse/issues/250) / closed | 静态确认 | 首轮已修复 F02 明确子项；范围/限制见前表 | F02 | 最终参数分支仍把 -Embedding 等作为目录。 |
| [#249 Takeover takes over shell-namespace windows it cannot render (phone over MTP)](https://github.com/jimmgreen/pulse/issues/249) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E03 | 明确过滤非文件系统 namespace；手机窗口本次未实际连接验证。 |
| [#248 Control Panel launches Pulse with -Embedding, which is parsed as a folder path (system32\-Embedding)](https://github.com/jimmgreen/pulse/issues/248) / closed | 静态确认 | 首轮已修复 F02 明确子项；范围/限制见前表 | F02 | 仅确认 OLE 参数误解析；采用评论更正，Control Panel 触发来源是第三方残留。 |
| [#246 右键菜单问题](https://github.com/jimmgreen/pulse/issues/246) / closed | 缺少条件 | 未实施修复，保留原判定 | src/app/context_menu_prefs.cpp:139-151 保存 seen 项，352-390 合并旧键；旧版评论声称迁移到实时系统列表不自动构成本地运行证据。；本次没有 Avira 卸载前后目录/注册残留或 Notepad++ 版本对照。 | 需卸载前后实际扩展注册/设置目录快照及 Notepad++ 安装类型；未将保留 seen 机制直接认定为当前 UI 一定残留。 |
| [#245 安卓手机通过数据线接入电脑后，Pulse读取有问题](https://github.com/jimmgreen/pulse/issues/245) / open | 静态确认 | 未实施修复，保留原判定 | src/fs/fs_enum.cpp:468-488 的 This PC 只按 GetLogicalDrives 生成盘符项；525-556 目录路径只走 NT/Win32 枚举。；src/app/app_model.cpp:1322 设备列表仍为逻辑盘；对 src/app、src/fs 检索未找到 PortableDevice/WPD/MTP/EnumObjects 枚举后端。；src/app/explorer_window_takeover.cpp:140-154 拒绝非文件系统 namespace；过滤接管并未实现手机浏览。 | 确认当前缺标准 MTP namespace 枚举/浏览能力；未连接 Android 手机复现空目录，不能扩大为所有 USB 盘模式都不支持。上游将发布支持的评论不是本地实现证据。 |
| [#241 Listary不会自动跳转的问题](https://github.com/jimmgreen/pulse/issues/241) / open | 缺少条件 | 未实施修复，保留原判定 | sources/241-readable.json 未提供 Listary 版本/配置；评论对适配责任有不同判断。；没有通过改注册表、伪装窗口类或启动外部软件来猜测兼容。 | 需 Listary 的实际版本、跳转入口/适配配置和当前窗口识别记录；现有材料不能判断 Pulse 或 Listary 谁负责。 |
| [#237 nas映射的磁盘，文件无法删除](https://github.com/jimmgreen/pulse/issues/237) / open | 缺少条件 | 未实施修复，保留原判定 | sources/237-readable.json 只说明 NAS 映射盘复制进去后无法删除，缺设备、协议和返回码。；环境虽有 Z: Network/NTFS，但不能据此认定与报告相同 NAS。src/ops/delete_operation.cpp:575 有回收失败后的永久删除授权流程。 | 需相同 NAS 设备/映射协议/权限及隔离测试文件的删除返回；本次未删除网络映射盘数据。 |
| [#236 对局域网网络文件夹支持不是很友好](https://github.com/jimmgreen/pulse/issues/236) / open | 运行复现 | 首轮已修复 F20 明确子项；范围/限制见前表 | run.log：非持久化 PlacesCatalog 加入虚构、未连接的 UNC 项，生产 BuildVm/HitTest 找到网络侧栏行，再实际调用 HandleRButtonUp；s.menu 仍为空，无菜单。；src/app/app_input.cpp:3431 SidebarItem 只为 QuickAccess/Starred/内置快捷项显示菜单；places.cpp:706 只有 PinNetwork，未见对应取消钉住方法。settings_controller.cpp:622 删除网络索引 root，不移除 places.networks。 | 只确认已钉网络项右键/移除入口缺失，不包括局域网自动发现功能诉求；虚构 UNC 未做任何网络访问，实际 SMB 连接不影响此菜单分支。 |
| [#235 将PDF文件拖放到外部应用时无反应](https://github.com/jimmgreen/pulse/issues/235) / closed | 静态确认 | 首轮已修复 F04 明确子项；范围/限制见前表 | F04 | 主窗口外拖没有允许 LINK，LINK-only 目标无可接受效果。 |
| [#234 重命名鼠标跳转问题](https://github.com/jimmgreen/pulse/issues/234) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/edit_host.cpp:108; src/ui/lumatext_renderer.cpp:1039；原文“鼠标跳转”含义不明；评论按输入光标解读。 | 需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。 实际鼠标指针本身是否跳动仍需明确步骤。 |
| [#230 用 Pulse 代替文件资源管理器的开关都打开了了，win+E打开的是Windows资源管理器，不知道是不是Windows预览版的原因，26340.9616](https://github.com/jimmgreen/pulse/issues/230) / open | 缺少条件 | 未实施修复，保留原判定 | sources/230-readable.json 的标题/评论明确 Windows 预览版 26340.9616 升级后 Win+E 回到 Explorer。；当前环境 inventory 为26300.9457，与原 OS build 不同。src/app/default_file_manager.cpp:73-76 有独立 Directory/WinE 项。 | 需26340.9616及实际接管注册/Win+E链路；本次未更改注册表或系统接管，不能确认是否为 OS 回归。 |
| [#229 C#编译的程序修改图标后不变](https://github.com/jimmgreen/pulse/issues/229) / open | 运行复现 | 首轮已修复 F25 明确子项；范围/限制见前表 | 同路径EXE红蓝资源实际变更；生产ConvertShellIconPixels转像素；friend Seed注入红缓存后，生产RequestExact仍返回相同index/hash11229969851736765315；从当前蓝资源新建独立cache hash11291544229467526019；src/ui/shell_icons.cpp:231-233,424-427只按path命中；全src未找到文件变化后调用图标缓存按路径失效/Clear/Reset | Seed绕过worker和IPC；没有端到端验证CSC/外部更新/监视通知/UI；本组运行复现限定生产缓存命中逻辑 |
| [#228 [Bug] 设置为默认文件管理器后，桌面双击文件夹错误触发第三方程序](https://github.com/jimmgreen/pulse/issues/228) / open | 缺少条件 | 未实施修复，保留原判定 | sources/228-readable.json 提供 Explorer→VSLauncher→devenv 和禁用 AnyCode 后转 Git GUI 的原环境轨迹，作者自己仍称根因未定。；src/app/default_file_manager.cpp:73 的 Directory 组及112-221备份/冲突保护路径需要结合原注册状态判断。 | 需原 HKCU/HKLM Directory 默认动作、隐藏标记、关联合并和 Procmon 重放；本次未临时禁用 AnyCode、修改注册表或启动桌面默认动作。 |
| [#226 无法访问服务器](https://github.com/jimmgreen/pulse/issues/226) / open | 缺少条件 | 未实施修复，保留原判定 | 原报告要求服务器离线时弹用户名/密码；src/app/app_navigation.cpp:154 只有 UNC 后台探测，src 未检出生产 CredUI/WNetAddConnection 交互认证流程（测试文件的 WNetAddConnection 不计）。；没有对用户既有 Z: 网络映射尝试重连、断开、改凭据或复用报告中的服务器。 | 需要可控的要求认证 SMB 服务器、全新凭据会话、UNC 状态与错误码；未把缺交互入口静态迹象扩大成已复现全部服务器访问失败。 |
| [#225 三个问题，希望能快速处理](https://github.com/jimmgreen/pulse/issues/225) / open | 运行复现 | 未实施修复，保留原判定 | bench_data/issue-confirm-external/run.log：普通文件夹选择能得到规范化文件路径；切换同一 snapshot 到 ArchiveViewPath 后 SelectedCount=1 但 EntryFullPath/SelectedFullPaths 都为空。；src/app/app_runtime.cpp:2242,2254-2263 清空压缩包来源；src/app/app_input.cpp:472-473 在 OLE 之前对空路径返回。 | 确认内置压缩包视图直接拖出的阻断；没有真实 DoDragDrop/解压写盘，也没有验证第三方压缩软件虚拟 IDataObject 拖入。原文另外两项建议不纳入故障结论。 |
| [#222 不能自动定位到文件](https://github.com/jimmgreen/pulse/issues/222) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E03 | 已有接管文件选中状态和稳定等待；浏览器实际调用链未验证。 |
| [#220 绿色版没安装索引服务但也会启动pulse.index.exe](https://github.com/jimmgreen/pulse/issues/220) / open | 行为已确认但缺陷未定 | 未实施修复，保留原判定 | src/index/index_client.cpp:63 SpawnHelper 在没有安装服务时允许启动同目录 Pulse.Index.exe 用户进程，这是已有用户级索引路径。；原文正文为空，作者评论提到总开关；本次没有改变索引设置、停止用户现有进程或启动真实 helper。 | 没安装 Windows 服务不等于禁止用户级索引进程。需用户确认预期及索引总开关的实际状态/版本/命令行；未验证关闭总开关后仍重启的独立故障。 |
| [#218 [稳定性/资源泄漏] NetworkAgentClient 每秒轮询导致 Timer 与 Semaphore 内核句柄持续泄漏](https://github.com/jimmgreen/pulse/issues/218) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E13 | 报告中的每请求 Deadline 短命线程已经不在；原泄漏因果存在争议，未做句柄时间序列。 |
| [#214 右键还是没有winrar](https://github.com/jimmgreen/pulse/issues/214) / closed | 缺少条件 | 未实施修复，保留原判定 | src/shell_host/ctx_handlers.cpp:253-292 枚举传统 handlers；src/shell_host/main.cpp:1141 起逐 handler 路径存在特定包扩展覆盖风险。；sources/214-readable.json 的修复声明用 Bandizip 代测，评论也说未装 WinRAR/7-Zip；不能用它验收原软件。 | 需报告者 WinRAR/7-Zip 实际版本、传统/MSIX安装类型和文件夹菜单；本次未安装或调用压缩扩展。 |
| [#213 文件图标和缩略图错误](https://github.com/jimmgreen/pulse/issues/213) / open | 静态确认 | 首轮已修复 F08 明确子项；范围/限制见前表 | F08 | alpha 类型仍被丢弃；7Z 小图标与 PSD 失效不统一认定为同因。 |
| [#212 手势软件功能失效](https://github.com/jimmgreen/pulse/issues/212) / open | 缺少条件 | 未实施修复，保留原判定 | docs/validation/issue-confirm-2026-10-23/sources/212-readable.json | 需Wgestures实际版本和真实手势注入环境；未安装/修改软件或用模拟消息声称兼容。 |
| [#211 重命名文件的时候，录入文字的过程中，光标会自动往前跳](https://github.com/jimmgreen/pulse/issues/211) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/edit_host.cpp:108; src/ui/lumatext_renderer.cpp:1039；维护者早期修复评论与后续同族报告均已阅读；评论不是本次验收证据。 | 需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。 |
| [#209 电脑接入Android手机无法正常识别](https://github.com/jimmgreen/pulse/issues/209) / open | 静态确认 | 未实施修复，保留原判定 | 与 #245 同源：src/fs/fs_enum.cpp:468-488,525-556 只支持逻辑盘/NT/Win32 路径，src/app/app_model.cpp:1322 同为逻辑盘。；sources/209-readable.json 的手机支持评论属于上游将发布声明；当前没有该后端/设备分组实现。 | 确认标准 Android MTP 设备不能由当前枚举路径提供；未用实体手机复现，评论另提 Send to 仍需完整菜单检查。 |
| [#208 此电脑中，网络磁盘的磁盘名称无法修改](https://github.com/jimmgreen/pulse/issues/208) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E16 | 本地存在独立 drive_label_rename 模块；网络映射盘显示名重命名未实测。 |
| [#205 对 Bandzip 右键菜单不支持](https://github.com/jimmgreen/pulse/issues/205) / closed | 缺少条件 | 未实施修复，保留原判定 | sources/205-readable.json 为 Bandizip 菜单缺失，原文只有截图、未注明版本；维护者测试机的修复声明不是当前安装证据。；传统 handler 枚举路径存在，包扩展则有原审计 F13 的条件性风险。 | 需 Bandizip 具体版本/安装类型、目标文件类型及完整原生/Pulse菜单对照；未执行扩展动作。 |
| [#200 新建文件(夹)后不能自动选中重命名](https://github.com/jimmgreen/pulse/issues/200) / open | 缺少条件 | 未实施修复，保留原判定 | src/app/app_commands.cpp:137 CreateNewItem 先用 PromptNewItemName 获取最终名称，再提交创建；不是必然先创建再弹行内改名。；src/app/app_ops_ui.cpp:43 RevealPendingCreatedItem 有待创建名称及 RemapSelection 的选择/可见恢复机制。原 issue 正文为空。 | 需要报告者实际入口（新建按钮/菜单/Shell 模板）、文件类型与期望命名时序；本次未操作新建对话框，不能把 UI 设计差异判成选择失效。 |
| [#199 关于无论咋样都更新不了新版本的问题](https://github.com/jimmgreen/pulse/issues/199) / open | 缺少条件 | 未实施修复，保留原判定 | 本轮已读 sources/199-readable.json；报告为从 1.0.51 更新失败并卡顿，评论称跳过后仍是旧版本。；没有在用户安装目录运行更新、替换文件或终止进程。 | 需要失败时下载/替换日志、旧版本原始包、占用文件与可写权限，在隔离更新目录复测卡顿和失败；不从当前版本号推测原因。 |
| [#197 应用窗口位置bug](https://github.com/jimmgreen/pulse/issues/197) / open | 静态确认 | 首轮已修复 F09 明确子项；范围/限制见前表 | F09 | 会话窗口坐标未按当前显示器约束。 |
| [#196 设置页的滚动条存在异常，只能通过滚轮滚动无法通过左键拉动](https://github.com/jimmgreen/pulse/issues/196) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E10 | 有 settings scrollbar 命中及拖动计算分支；未视觉操作。 |
| [#194 小窗模式下，会出现顶部标签页与设置、明暗按钮的UI重叠](https://github.com/jimmgreen/pulse/issues/194) / open | 定向未复现 | 未实施修复，保留原判定 | bench_data/issue-confirm-ui/visual-manifest.json；bench_data/issue-confirm-ui/runtime.log；src/ui/ui_renderer_internal.h:1418；19标签，320/420/600/800 DIP，100%/200%，深浅16帧；48个最小化/最大化/关闭命中检查全部为1。标题栏设置/主题宽度为0，设置在侧栏。 | 当前生产自绘布局未见原述标签与顶部设置/主题重叠；离屏测试未替代真实窗口原生非客户区验收。 |
| [#192 没有右键：发送到桌面快捷方式的选项](https://github.com/jimmgreen/pulse/issues/192) / closed | 静态确认 | 未实施修复，保留原判定 | F06 | 发送到初始化/空目标失效条件仍在；桌面目标本机未验证。 |
| [#188 反馈几个bug和建议](https://github.com/jimmgreen/pulse/issues/188) / open | 运行复现 | 首轮已修复 F17 明确子项；范围/限制见前表 | bench_data/issue-confirm-ui/runtime.log；src/app/address_search.cpp:188；src/app/address_search.cpp:194；content空串live查询后仍为content:needle、snapshot为空、search_content_empty=true，未返回origin；文件名空串Enter保留旧查询，live仍为search虚拟路径。 | 已复现故障#2（删空后未恢复目录）的生产状态转换；未称完整可见窗口/索引结果排序已复现。故障#1长名输入乱跳仍缺真实IME；建议段不计bug。 |
| [#187 反馈几个bug和建议](https://github.com/jimmgreen/pulse/issues/187) / closed | 运行复现 | 首轮已修复 F17 明确子项；范围/限制见前表 | bench_data/issue-confirm-ui/runtime.log；src/app/address_search.cpp:188；src/app/address_search.cpp:194；content空串live查询后仍为content:needle、snapshot为空、search_content_empty=true，未返回origin；文件名空串Enter保留旧查询，live仍为search虚拟路径。 | 已复现故障#2（删空后未恢复目录）的生产状态转换；未称完整可见窗口/索引结果排序已复现。故障#1长名输入乱跳仍缺真实IME；建议段不计bug。 |
| [#186 反馈几个bug和几个建议](https://github.com/jimmgreen/pulse/issues/186) / closed | 运行复现 | 首轮已修复 F17 明确子项；范围/限制见前表 | bench_data/issue-confirm-ui/runtime.log；src/app/address_search.cpp:188；src/app/address_search.cpp:194；content空串live查询后仍为content:needle、snapshot为空、search_content_empty=true，未返回origin；文件名空串Enter保留旧查询，live仍为search虚拟路径。 | 已复现故障#2（删空后未恢复目录）的生产状态转换；未称完整可见窗口/索引结果排序已复现。故障#1长名输入乱跳仍缺真实IME；建议段不计bug。 |
| [#185 反馈几个bug和几个建议](https://github.com/jimmgreen/pulse/issues/185) / closed | 运行复现 | 首轮已修复 F17 明确子项；范围/限制见前表 | bench_data/issue-confirm-ui/runtime.log；src/app/address_search.cpp:188；src/app/address_search.cpp:194；content空串live查询后仍为content:needle、snapshot为空、search_content_empty=true，未返回origin；文件名空串Enter保留旧查询，live仍为search虚拟路径。 | 已复现故障#2（删空后未恢复目录）的生产状态转换；未称完整可见窗口/索引结果排序已复现。故障#1长名输入乱跳仍缺真实IME；建议段不计bug。 |
| [#184 [BUG反馈] 选中 .srt 文件时右侧详细信息侧边栏异常遮挡其他界面元素](https://github.com/jimmgreen/pulse/issues/184) / open | 缺少条件 | 未实施修复，保留原判定 | PreviewHandlerHost使用WS_POPUP overlay和条件HWND_TOPMOST；.srt未列入原生文本白名单 | 真实srt关联provider；打开交互面板后的重叠和输入验证；不能由overlay设计单独推出原故障 |
| [#183 反馈几个bug和几个建议](https://github.com/jimmgreen/pulse/issues/183) / closed | 运行复现 | 首轮已修复 F17 明确子项；范围/限制见前表 | bench_data/issue-confirm-ui/runtime.log；src/app/address_search.cpp:188；src/app/address_search.cpp:194；content空串live查询后仍为content:needle、snapshot为空、search_content_empty=true，未返回origin；文件名空串Enter保留旧查询，live仍为search虚拟路径。 | 已复现故障#2（删空后未恢复目录）的生产状态转换；未称完整可见窗口/索引结果排序已复现。故障#1长名输入乱跳仍缺真实IME；建议段不计bug。 |
| [#181 [bug] 预览文件在窗口焦点切出pulse后不显示](https://github.com/jimmgreen/pulse/issues/181) / open | 静态确认 | 未实施修复，保留原判定 | src/app/app_main.cpp:1013-1030传递激活状态；src/ui/preview_handler_host.cpp:545-550显式SWP_HIDEWINDOW；src/ui/preview_handler_host.cpp:863-873,1097-1103状态发布与同identity重放位置 | 真实provider切焦未运行；显式隐藏为overlay置顶策略，不等于provider失效；是否符合产品预期仍待决定；不泛化到原生预览 |
| [#179 【BUG反馈】在桌面剪切复制文件，没办法粘贴到pulse文件夹里](https://github.com/jimmgreen/pulse/issues/179) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E05 | 普通 HDROP 和虚拟 Shell 文件均有路径；桌面剪切粘贴尚未实操。 |
| [#176 右键菜单设置与实际显示不一致](https://github.com/jimmgreen/pulse/issues/176) / closed | 缺少条件 | 未实施修复，保留原判定 | sources/176-readable.json 为设置开关与实际菜单不同；评论称实时目录重构，不能代替本地菜单返回验证。；src/app/context_menu_controller.cpp:393-404 的 partial 缓存和 context_menu_prefs.cpp:139-151 的 seen 项均需实际配置才可归因。 | 需具体勾选项目、handler注册、返回菜单与设置数据的同一时点快照；本次只读源码，没有改用户配置。 |
| [#175 右键git菜单经常被自动关掉](https://github.com/jimmgreen/pulse/issues/175) / open | 缺少条件 | 未实施修复，保留原判定 | sources/175-readable.json 描述 Git 菜单经常自动关闭，未提供事件步骤/扩展版本。；src/ui/fluent_menu.cpp:365-373,1880 有失活关闭路径，不能仅凭存在关闭逻辑认定异常。 | 需菜单打开到关闭的激活/焦点/异步完成时间线和 Git 扩展状态；本次未打开真实 Git 菜单或模拟系统失焦。 |
| [#173 列宽问题](https://github.com/jimmgreen/pulse/issues/173) / open | 运行复现 | 首轮已修复 F23 明确子项；范围/限制见前表 | bench_data/issue-confirm-external/run.log：普通Details共4列，Size右外缘x=1062命中ColumnHeader(56)，内部Type/Size分界命中ColumnDivider(57)。；src/ui/ui_hit_test.cpp:896 只检查count-1个内部边；src/ui/ui_pane_list.cpp:567 拒绝末列外缘索引，私有调用返回原始宽度数组。 | 确认末列右外缘不能向左拖；未对原截图1411x753进行像素复现，也没有物理鼠标拖动或安装版验收。 |
| [#169 文件夹位置问题](https://github.com/jimmgreen/pulse/issues/169) / open | 运行复现 | 首轮已修复 F19 明确子项；范围/限制见前表 | 原文明确“打开文件夹，再返回，文件夹位置不是原来的位置”。生产 Tab::NavigateTo→GoBack 清 scroll，RestoreNavigationReturnSelection 只恢复子目录并 EnsureRowVisible。；run.log：LargeIcons、240 项，父目录选择索引75，进入子目录前 scroll=1800；返回后选择仍75，scroll=1592。 | 确认原视口偏移不保留；没有声称返回时丢失选择或排序次序变化。只覆盖一组 100%/1000×700 网格几何，未按截图尺寸逐像素复刻。 |
| [#166 设置的滚动条不能拖动](https://github.com/jimmgreen/pulse/issues/166) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E10 | 设置滚动条已有拖动分支；远程触摸拖文件另属触摸验证范围。 |
| [#165 我的还是不能右键打开终端呀,可以用自带的,不能使用pulse的](https://github.com/jimmgreen/pulse/issues/165) / open | 缺少条件 | 未实施修复，保留原判定 | src/ops/ops_manager.cpp:1140-1148 的内置入口使用 wt.exe -d；环境能找到 WindowsApps/wt.exe alias。；sources/165-readable.json 评论里作者说命令行可运行，不能简单归因为未安装Terminal。 | 需原会话环境、内置菜单实际启动错误/HRESULT与alias可执行行为；本次未打开终端，不把alias存在当启动成功。 |
| [#162 [BUG]重命名时首次输入时输入焦点丢失且选词后光标移动到最左侧；[建议]预览文件时按照真实文件头进行判定。](https://github.com/jimmgreen/pulse/issues/162) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/edit_host.cpp:108; src/ui/lumatext_renderer.cpp:1039；原文34MB GIF无本地原样本；按实际文件头判定的建议单独排除。 | 需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。 大GIF失败需原样本和预览宿主验证，UI组未宣称修复。 |
| [#154 新版本中，zip文件右键时找不到解压缩选项](https://github.com/jimmgreen/pulse/issues/154) / closed | 缺少条件 | 未实施修复，保留原判定 | sources/154-readable.json 的评论确认截图关联为 NanaZip；原报告要求zip解压菜单。；私有静态 .zip 枚举只有 openwith/openas，不包含 COM；传统/包扩展覆盖差异仍需具体安装。 | 需 NanaZip版本及zip完整菜单，不能用本机静态动词缺 Extract 或维护者Bandizip测试宣称复现/解决。 |
| [#151 快捷方式显示为*.lnk](https://github.com/jimmgreen/pulse/issues/151) / open | 行为已确认但缺陷未定 | 未实施修复，保留原判定 | bench_data/issue-confirm-ui/layout-1600-200-dark.png；src/ui/ui_renderer_internal.h:169；生产展示模型有意保留on-disk .lnk；隔离DirEntry名称Example shortcut.lnk在截图中保留后缀。 | 只确认显示层现象；产品约定未定，不计新增bug。未创建/打开真实Shell快捷方式或修改隐藏扩展名偏好。 |
| [#150 [bug]关于标签问题](https://github.com/jimmgreen/pulse/issues/150) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E16 | tags_loaded 区分空列表与从未保存，已删除默认标签不会因空而自动恢复。 |
| [#147 搜索结果中没有过滤掉不符合要求的文件，以及搜索出来的文件无法空格预览。](https://github.com/jimmgreen/pulse/issues/147) / open | 定向未复现 | 未实施修复，保留原判定 | 生产SelectedQuickPreviewItem返回含空格绝对路径；生产CollectFilterMatches对两项仅保留匹配项；src/app/app_input.cpp:3693-3694空格调用ToggleQuickPreview | 未实发空格或打开实际预览；搜索框输入路由、焦点、流式结果、内容分页未验收 |
| [#146 [BUG] 此电脑/磁盘根目录点击顶部「向上一级」按钮，程序崩溃退出（附崩溃转储，1.0.53 必现）](https://github.com/jimmgreen/pulse/issues/146) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E21 | 空路径 ParentPath 安全返回，未发现报告所述非法路径异常；未真实点击回归。 |
| [#142 图片预览有卡顿，且大图片预览不显示](https://github.com/jimmgreen/pulse/issues/142) / open | 缺少条件 | 未实施修复，保留原判定 | src/preview_host/main.cpp:304,753-778存在WIC及图片回退路径 | 原图尺寸/编码/位深；解码错误、内存和延迟测量；未用普通小图片代替原件 |
| [#140 对图片文件右键没有打印按钮](https://github.com/jimmgreen/pulse/issues/140) / closed | 缺少条件 | 未实施修复，保留原判定 | src/app/shell_verbs.cpp:77 有print显示名称；私有静态 .png 枚举5项没有print。；该夹具没有构造原生或Pulse COM菜单，因此没有证实最终图片菜单缺打印。 | 需原图片关联、png/报告图片类型以及完整COM菜单对照；本次未调用打印。 |
| [#138 暗色设置单选按钮选中文字看不清](https://github.com/jimmgreen/pulse/issues/138) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/ui_settings_core.cpp；bench_data/issue-confirm-environment/environment.json | 原文指定Win10暗色单选框；当前非Win10环境，普通深色目录截图不代替设置单选控件验收。 |
| [#136 建议优化重命名功能的位置定位，降低灵敏度和取消自动回正](https://github.com/jimmgreen/pulse/issues/136) / open | 运行复现 | 首轮已修复 F18 明确子项；范围/限制见前表 | bench_data/issue-confirm-ui/rename-end.bmp；bench_data/issue-confirm-ui/rename-click-middle.bmp；bench_data/issue-confirm-ui/runtime.log；src/ui/lumatext_renderer.cpp:833；生产CallLumaEditMouse处理编辑器中部单击：caret由194转182，重绘后尾部_END.txt消失；EditScrollX按每次caret从0重新算偏移，已有横向位置不保持。 | 仅长名鼠标点选后视口跳动子步骤；未复现原中文文件名整段拖动和灵敏度，没有真实桌面全局输入。 |
| [#135 删除文件BUG及空格预览视频文件bug](https://github.com/jimmgreen/pulse/issues/135) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/video_preview.cpp:328 MFPlay播放路径；已将TF删除与视频卡顿分开记录 | TF介质与删除原错误码；原卡顿视频及播放测量；本组未做TF删除 |
| [#129 文件位置问题](https://github.com/jimmgreen/pulse/issues/129) / open | 运行复现 | 首轮已修复 F19 明确子项；范围/限制见前表 | 原文要求“父文件夹位置不在打开前的位置”，并提到铺满窗口时滚到底。与 #169 共用生产导航/返回定位 fixture。；选中恢复75成功，但同一目录返回 scroll 1800→1592，证实原位置不恢复；app_model.cpp:512 GoBack 清偏移，app_navigation.cpp:1547 仅保证子目录可见。 | 只复现视口位置变化，未复现字面上的“滚到整个目录最底部”；不能据该偏移变化宣称最后一行跳转已重现。 |
| [#126 临时暂存托盘已经影响复制，剪切了！求关掉](https://github.com/jimmgreen/pulse/issues/126) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E05 | 外部较新剪贴板与托盘批次有序号决策；未重现“单击就进入托盘”现象。 |
| [#124 【BUG 描述】文件移动后原有标签检索提示文件不可用](https://github.com/jimmgreen/pulse/issues/124) / open | 静态确认 | 未实施修复，保留原判定 | F12 | 外部移动且未浏览新目录时，标签仍从旧路径账本枚举。 |
| [#122 索引服务无法安装](https://github.com/jimmgreen/pulse/issues/122) / open | 缺少条件 | 未实施修复，保留原判定 | 已读本轮正文和全部缓存评论：1.0.53 升级后拒绝访问、管理员重装仍失败，也有用户报告等待连接/结束进程后恢复。；本次未安装/卸载/停止索引服务，未杀用户进程，未用当前环境的正常状态否定历史升级故障。 | 需要原版本/升级残留、服务账户和权限、端点/进程版本与安装错误码，在隔离可回滚环境复现；拒绝访问原因目前未定。 |
| [#116 使用中文重命名文件时，光标会出现在最前面，导致输入的字符顺序有问题。](https://github.com/jimmgreen/pulse/issues/116) / open | 缺少条件 | 未实施修复，保留原判定 | src/ui/edit_host.cpp:108; src/ui/lumatext_renderer.cpp:1039；已读评论中“初次修复后后续词组仍偶发”的反馈；不能仅验证首次选区。 | 需要真实微软拼音连续词组、首次替换选区及中英文数字模式；未使用WM_CHAR/WM_IME_COMPOSITION模拟替代真实IME验收。 |
| [#115 wps打开文件位置功能不工作，提示“Windows无法访问指定设备、路径或文件。你可能没有适当的权限访问该项目。”](https://github.com/jimmgreen/pulse/issues/115) / open | 缺少条件 | 未实施修复，保留原判定 | sources/115-readable.json 为 WPS 打开文件位置提示访问被拒，未提供文件路径/权限/具体WPS版本。；系统集成/默认动作与路径权限均可能参与，静态代码不能独立归因。 | 需同版本WPS、原路径与 Shell调用/权限记录；本次未启动WPS或改变系统关联。 |
| [#109 从windows 遠端桌面(Remote desktop) 复制档案，无法在pulse 中贴上](https://github.com/jimmgreen/pulse/issues/109) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E05 | 虚拟剪贴板 Shell 粘贴有实现；真实远程桌面/Smart Data Link 未验证。 |
| [#106 Recycle Bin: column widths cannot be dragged or refitted](https://github.com/jimmgreen/pulse/issues/106) / open | 静态确认 | 首轮已修复 F05 明确子项；范围/限制见前表 | F05 | 回收站绘制和拖动/适配使用不同列宽数组。 |
| [#94 右键菜单没有创建快捷方式？还有原文件管理器的右键拖拽可选处理方式功能也没有。](https://github.com/jimmgreen/pulse/issues/94) / open | 静态确认 | 未实施修复，保留原判定 | src/app/app_input.cpp:2140-2143 仅左键行点击设置dragPending；1604-1617要求VK_LBUTTON才启动StartDragOut。；同文件3321-3358的HandleRButtonDown只预取上下文菜单；ui/drag_drop.cpp:549虽接受MK_RBUTTON释放但没有右键启动调用点。 | 仅确认右键拖拽选择复制/移动/快捷方式的入口缺失；创建快捷方式菜单及FastCopy/Send to仍需具体完整菜单验证，未统一认定都缺失。 |
| [#92 OGG格式无法预览，及音乐相关问题](https://github.com/jimmgreen/pulse/issues/92) / open | 缺少条件 | 未实施修复，保留原判定 | 已读四点正文与评论；生产路径仍MFPlay，全src未找到FFmpeg播放增强包实现 | Ogg内部编码与实际文件、系统codec；播放与磁盘容量刷新运行验证；不将评论修复宣称当成本地实现 |
| [#88 符号链接与 junction 被当作普通文件夹显示（无类型与图标区分）](https://github.com/jimmgreen/pulse/issues/88) / closed | 静态确认 | 未实施修复，保留原判定 | F07 | 目录类型直接显示文件夹，未区分 symlink/junction 或解析其目标。 |
| [#87 网络位置如何添加？](https://github.com/jimmgreen/pulse/issues/87) / open | 运行复现 | 首轮已修复 F20 明确子项；范围/限制见前表 | 原文后续评论明确已找到网络项添加方法，但“已添加网络位置右键没有反应、如何删除”。；与 #236 同一生产右键 fixture：非持久化 pinned UNC 行可命中，HandleRButtonUp 不创建菜单；设置的 RemoveNetwork 仅作用索引 roots。 | 只计入右键/取消钉住故障；不把继承 Explorer 网络发现/WPS 云盘展示等新增能力诉求算 bug。 |
| [#86 右键菜单无法启动第三方卸载软件](https://github.com/jimmgreen/pulse/issues/86) / closed | 静态确认 | 首轮已修复 F14 明确子项；范围/限制见前表 | F14 | 需要提权的第三方 command 仍可能在 CreateProcess 返回 740 后直接失败。 |
| [#81 设为默认文件资源管理器后的问题](https://github.com/jimmgreen/pulse/issues/81) / closed | 缺少条件 | 未实施修复，保留原判定 | sources/81-readable.json 后续明确是This PC点盘符失败，侧树仍可用；不能只按最初默认管理器还原标题判断。；src/app/default_file_manager.cpp:188-221,279 有旧版本迁移/备份还原流程，未检验原注册初值。 | 需开启/取消前后的用户关联快照、This PC盘符默认动作和真实Shell失败信息；本次不修改系统接管/注册表。 |
| [#77 [BUG反馈] 右键菜单中的“发送到”没有二级子菜单且点击无反应](https://github.com/jimmgreen/pulse/issues/77) / closed | 静态确认 | 未实施修复，保留原判定 | F06 | 共享截止时间、普通占位/空子菜单与慢扩展机制仍有同类缺口。 |
| [#74 [BUG反馈] 网络位置/SMB共享路径下搜索无结果](https://github.com/jimmgreen/pulse/issues/74) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E14 | 未覆盖 UNC 可走后台实时文件名遍历；实际 SMB 搜索未验证。 |
| [#73 [bug反馈] 在文件夹空白处右键菜单不显示新建菜单](https://github.com/jimmgreen/pulse/issues/73) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E01 | native New 顶层初始化已豁免预算；冷缓存菜单原位插入仍需实际验证。 |
| [#72 [BUG] 版本更新后固定到任务栏的pulse会失效](https://github.com/jimmgreen/pulse/issues/72) / closed | 静态确认 | 未实施修复，保留原判定 | F15 | 安装器仍先静默卸载，缺同目录原地覆盖分支。 |
| [#66 [安全] 索引服务管道对所有已认证用户开放，检索结果不套用 NTFS ACL（多用户机器信息泄露）](https://github.com/jimmgreen/pulse/issues/66) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E12 | 存在调用者可见性过滤和受限索引目录 ACL；未做多账户服务验收。 |
| [#65 [稳定性] 卡住的第三方右键扩展会永久泄漏 pulse_shell 线程（INFINITE join 无法回收）](https://github.com/jimmgreen/pulse/issues/65) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E18 | 菜单会话可由父进程终止，隔离架构已改变；未注入挂死扩展运行。 |
| [#60 [性能] 从压缩包拖拽复制明显慢：无冲突时不必绕临时文件（附实测与取舍）](https://github.com/jimmgreen/pulse/issues/60) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E06 | 无冲突小文件已有直接写最终名分流，未重测吞吐。 |
| [#59 [BUG] 1.0.46 右键菜单第三方项时有时无：残缺的快速快照被写入缓存](https://github.com/jimmgreen/pulse/issues/59) / closed | 静态确认 | 首轮已修复 F01 明确子项；范围/限制见前表 | F01 | partial 快照仍会写入缓存并作为下一次底稿。 |
| [#58 按大小排序时，文件夹的大小排序错误，文件的大小排序正确](https://github.com/jimmgreen/pulse/issues/58) / closed | 静态确认 | 首轮已修复 F27 明确子项；范围/限制见前表 | run.log：私有目录 a-large 含1000字节文件，b-small 含10字节文件；生产 EnumerateDirectory 的目录 entry.size 均0，生产 EntryLess(Size,Asc) 结果 a-large 在 b-small 前。；src/app/entry_sort.cpp:87 直接比较 DirEntry.size；folder_sizes_ui.cpp:41 的统计仅为支持显示的视图准备请求，102 只写 folder_size_labels，未把 FolderSizes 的已统计 bytes 投影回排序。 | 这是目录枚举运行边界加统计未接排序的静态确认；未运行“等待真实 FolderSizes 全部完成后 UI 仍错误”的端到端试验，也未覆盖网络手动统计/三种文件夹排序策略。 |
| [#52 [BUG] 标题栏窗口按钮图标重复绘制且错位重叠（最小化/最大化/关闭）](https://github.com/jimmgreen/pulse/issues/52) / closed | 缺少条件 | 未实施修复，保留原判定 | src/app/app_main.cpp:2319；bench_data/issue-confirm-ui/visual-manifest.json；当前实际窗口仍用WS_OVERLAPPEDWINDOW（含WS_SYSMENU），离屏WS_POPUP fixture只验证自绘按钮。 | 原问题为DWM原生按钮透过材质叠影；需要真实标准样式、DWM材质和悬停截图。离屏自绘按钮正常不能宣布原bug已修复或已复现。 |
| [#51 大码流的视频几乎都没有办法播放，能不能调用 Quick Look？](https://github.com/jimmgreen/pulse/issues/51) / open | 缺少条件 | 未实施修复，保留原判定 | 评论要求XAVC 4K 50fps 10bit 4:2:2 280Mb/s；生产MFPlay路径静态存在 | 原始片段和系统codec；真实播放失败码与帧率；普通视频成功不能结案 |
| [#50 [Bug] 拖拽窗体不能触发Groupy](https://github.com/jimmgreen/pulse/issues/50) / open | 缺少条件 | 未实施修复，保留原判定 | src/app/app_main.cpp:2319；docs/validation/issue-confirm-2026-10-23/sources/50-readable.json | 需真实Groupy版本、窗口过滤与拖动合并；未安装软件或使用全局输入。 |
| [#49 [BUG] 双击 bat 一闪而过、没有真正执行：打开时没有把条目所在文件夹作为工作目录](https://github.com/jimmgreen/pulse/issues/49) / closed | 静态确认 | 首轮已修复 F03 明确子项；范围/限制见前表 | F03 | 普通打开未设置独立启动工作目录。 |
| [#48 [BUG] 文件和文件夹右键菜单缺少打包扩展（WinRAR），空白处正常](https://github.com/jimmgreen/pulse/issues/48) / closed | 静态确认 | 未实施修复，保留原判定 | F13 | 存在传统 handler 时，只在包清单注册的扩展未进入逐 handler 主路径。 |
| [#46 [BUG] VS Code 右键菜单项缺失](https://github.com/jimmgreen/pulse/issues/46) / closed | 静态确认 | 未实施修复，保留原判定 | F13 | MSIX-only 安装的类型缺支持；原 exe installer 版本仍需真实菜单确认。 |
| [#41 BUG 反馈：右键打不开终端，快速访问编辑徽章无法保存](https://github.com/jimmgreen/pulse/issues/41) / open | 已有针对实现（静态） | 未实施修复，保留原判定 | E16/E02/E07 | 徽章持久化和高级输入框回退有实现；终端失败另见 #165，且该 issue 尚有4条评论未取到。 |
| [#40 zip文件无法用pulse打开和空格预览问题](https://github.com/jimmgreen/pulse/issues/40) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E03/E08 | 本地已有独立压缩包浏览路径；旧 Office 进程残留需对应文档/provider，未验收。 |
| [#38 [BUG] nanazip右键菜单缺失](https://github.com/jimmgreen/pulse/issues/38) / closed | 静态确认 | 未实施修复，保留原判定 | F13 | 只依赖打包 IExplorerCommand 的 NanaZip 路径缺实现。 |
| [#29 右键[winrar压缩]的问题](https://github.com/jimmgreen/pulse/issues/29) / closed | 静态确认 | 未实施修复，保留原判定 | F13/F01 | 现代 WinRAR 的包注册与 partial 缓存均存在同类缺口；具体 WinRAR 版本未实装。 |
| [#20 窗体显示有点异常](https://github.com/jimmgreen/pulse/issues/20) / closed | 缺少条件 | 未实施修复，保留原判定 | src/ui/FluentTokens.h:244；docs/validation/issue-confirm-2026-10-23/sources/20-readable.json | 系统强调色标题栏开关与Mica/DWM实际组合未切换；离屏像素不含系统DWM底层着色。 |
| [#19 excel预览卡顿](https://github.com/jimmgreen/pulse/issues/19) / open | 缺少条件 | 未实施修复，保留原判定 | 已读后续复杂Excel闪白与滚动/拖动问题评论；当前Office Shell/缩略图路径；未见评论所称内置多sheet表格预览UI | 复杂原工作簿；真实provider及滚动/拖动测试；不得由早期评论或微小XLSX泛测关闭后续报告 |
| [#9 Previews that resolve through the shell thumbnail cost 40-53 ms on every request when no thumbnail can be produced](https://github.com/jimmgreen/pulse/issues/9) / closed | 定向未复现 | 未实施修复，保留原判定 | 生产StoreResult注入permanent failure后100次不同generation同key Draw均Failed且queue/pending为0；src/ui/thumbnail_cache.cpp:58,306缓存key与failure短路；src/preview_host/main.cpp:671-684仍有四路fallback | 失败由fixture注入，原XLSX/CAD-PDF样本未取得；未测得原40-53ms；直调host/冷缓存/像素尺寸变化/重启成本仍待测，不据此否认；未复现永久阻塞或所有后续预览失效 |
| [#4 设置里开启了打开方式的选项，但是右键菜单里并没有选择打开方式](https://github.com/jimmgreen/pulse/issues/4) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E02 | openas 静态项有兜底文字和独立系统 chooser 路径；未运行菜单验收。 |
| [#3 Filter box hides the text you type (the same hosted-editor presentation gap 1.0.13 fixed for rename)](https://github.com/jimmgreen/pulse/issues/3) / closed | 已有针对实现（静态） | 未实施修复，保留原判定 | E07 | 共享编辑宿主已重绘输入并支持原生回退；没有本次实际键入画面。 |

## 排除项对账

以下 19 条不纳入故障及待判定清单，仅保留编号和排除范围，不展开建议。#259 为正文复核后增加的排除项，其余 18 条来自候选筛选。

| Issue | 处理 |
|---|---|
| [#375](https://github.com/jimmgreen/pulse/issues/375) | 候选筛选已排除：非本次故障范围。 |
| [#367](https://github.com/jimmgreen/pulse/issues/367) | 候选筛选已排除：非本次故障范围。 |
| [#331](https://github.com/jimmgreen/pulse/issues/331) | 候选筛选已排除：非本次故障范围。 |
| [#293](https://github.com/jimmgreen/pulse/issues/293) | 候选筛选已排除：非本次故障范围。 |
| [#273](https://github.com/jimmgreen/pulse/issues/273) | 候选筛选已排除：非本次故障范围。 |
| [#259](https://github.com/jimmgreen/pulse/issues/259) | 正文复核为交互建议，移出故障范围。 |
| [#240](https://github.com/jimmgreen/pulse/issues/240) | 候选筛选已排除：非本次故障范围。 |
| [#193](https://github.com/jimmgreen/pulse/issues/193) | 候选筛选已排除：非本次故障范围。 |
| [#174](https://github.com/jimmgreen/pulse/issues/174) | 候选筛选已排除：非本次故障范围。 |
| [#172](https://github.com/jimmgreen/pulse/issues/172) | 候选筛选已排除：非本次故障范围。 |
| [#171](https://github.com/jimmgreen/pulse/issues/171) | 候选筛选已排除：非本次故障范围。 |
| [#170](https://github.com/jimmgreen/pulse/issues/170) | 候选筛选已排除：非本次故障范围。 |
| [#128](https://github.com/jimmgreen/pulse/issues/128) | 候选筛选已排除：非本次故障范围。 |
| [#111](https://github.com/jimmgreen/pulse/issues/111) | 候选筛选已排除：非本次故障范围。 |
| [#108](https://github.com/jimmgreen/pulse/issues/108) | 候选筛选已排除：非本次故障范围。 |
| [#79](https://github.com/jimmgreen/pulse/issues/79) | 候选筛选已排除：非本次故障范围。 |
| [#17](https://github.com/jimmgreen/pulse/issues/17) | 候选筛选已排除：非本次故障范围。 |
| [#16](https://github.com/jimmgreen/pulse/issues/16) | 候选筛选已排除：非本次故障范围。 |
| [#2](https://github.com/jimmgreen/pulse/issues/2) | 候选筛选已排除：非本次故障范围。 |
