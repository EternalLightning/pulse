# 预览定向审查与目录分发修复

日期：2026-10-21。范围为已有预览的缺陷与性能边界，不引入悬停卡片或其他预览功能。

## 已确认的问题

- 位置：`src/preview_host/main.cpp` 的 Content 请求分发。
- 触发：目录名称以 `.svg`、`.emf`、`.wmf` 结尾；请求属性包含 `FILE_ATTRIBUTE_DIRECTORY`。
- 原因：按扩展名选择矢量/图元渲染后，才处理目录。目录因此被作为图片文件打开。
- 修复前实测：通过真实 `Pulse.Preview.exe` 命名管道请求隔离目录，普通目录返回 `status=0 / Bitmap / 128x128`，`diagram.svg`、`diagram.emf` 均返回 `status=1 / Unsupported / 0x0`。
- 修复：目录属性优先使用原有 Shell 目录图标路径，再分发普通文件的扩展名。
- 回归：在现有 `pulse_preview_test --response-compatibility-only` 中增加普通目录和 `.svg`、`.emf`、`.wmf` 目录，检查真实进程响应、像素读取、尺寸以及夹具清理。

## 验证记录

- `git diff --check -- src/preview_host/main.cpp src/bench/preview_test_main.cpp`：通过。
- 修复后的真实 helper 定向请求：普通目录以及 `diagram.svg`、`diagram.emf`、`diagram.wmf` 均返回 `status=0 / Bitmap / 128x128`。
- 统一构建 `pulse_preview_test` 成功后，运行 `build/pulse_preview_test.exe --response-compatibility-only`：23 PASS、0 FAIL，退出码 0。覆盖普通位图的 32/512/1024 上限、四种目录名称、文本/二进制/属性/不可用内容的响应兼容以及隔离夹具清理。
- sandbox 内的命名管道测试曾返回 `Access to the path is denied`；修复前复现使用正常 Windows 权限完成，未将该环境错误判断为产品缺陷。
- 夹具仅在 `bench_data` 下新建隔离目录，复现结束后已清理。正式回归使用测试程序自身的独立临时目录。

## 其余审查与限制

- 检查 `quick_preview_window.cpp`、`quick_preview_playback.cpp` 的关闭/重开、切换与播放状态；检查 `preview_handler_host.cpp`、`video_preview.cpp` 的取消、工作线程配额、退役以及释放；按需检查相关预览 IPC 和解码分发。
- 未发现第二项有充分证据需要修改的问题；上述生命周期路径没有在本次重跑全部历史测试。
- 对“Shell 返回超过请求大小的位图”候选进行了真实目录请求检查；请求上限 32、33、48、96、129 均返回对应尺寸，未复现，因此未据此修改。
- 本次验证关注目录分发及响应协议，不声称第三方预览处理器、所有视频解码器、主题/DPI或实际界面操作已经重新验收。
