# 父仓库近期预览更新核对

核对时间：2026-10-21（北京时间）。通过 HTTPS 读取 `jimmgreen/pulse` 的 main；当时远端 HEAD 为 `c4fe01ed82fe091f495f531972a854aa32caac2b`（2026-10-16，1.0.56）。以下来自提交说明和对应源代码，未将上游性能数字视为本地验证结果。本次未合入这些预览更新。

| 日期 | 提交 | 预览变化 |
| --- | --- | --- |
| 10-16 | [1fdab04](https://github.com/jimmgreen/pulse/commit/1fdab0496a8a3e1b34df7d6c0e872f012f48d698) | 增强包自动更新开关；同时启用应用自动检查更新时，后台检查已安装、已启用的增强包。失败静默，用户取消后本轮不再自动重试。 |
| 10-15 | [11787b7](https://github.com/jimmgreen/pulse/commit/11787b72ccc21d2b5d895e09bcef72750ebd4d75) | 增强包出现“更新到”入口；按大小和 SHA-256 复用已有文件，仅下载需要变化的文件；保留用户选择的自有 FFmpeg 来源。 |
| 10-15 | [93c1bd0](https://github.com/jimmgreen/pulse/commit/93c1bd0e50f17821825f3ce8d27805760c5aebdb) | 解码器启动失败时抑制系统错误弹框，部分资源不足错误重试一次；新的 Radiance HDR 流式读取，支持更多头格式、方向和 RLE。 |
| 10-13 | [3d9fbea](https://github.com/jimmgreen/pulse/commit/3d9fbeab1344d5745bac6ed47aba407c5ba885e2) | 音频内嵌封面读取；HEIF 优先显示内嵌缩略图；视频先用 Explorer 缓存或 Media Foundation 关键帧；切走后放弃阻塞后续文件的慢解码。 |
| 10-13 | [40d44ed](https://github.com/jimmgreen/pulse/commit/40d44edc7e0aaadd3baa77c33c8ad5380de7d75b) | 指针停稳 150ms 后提前请求卡片预览；调整预览进程轮询；EXIF 图片先缩放后旋转；超预算 GIF 退为首帧。 |
| 10-13 | [0b86fec](https://github.com/jimmgreen/pulse/commit/0b86fecaeb5bc963235e51774bddf99193087bc4) | 卡片按图片、文档、文本或纯信息调整尺寸，并在形状之间过渡；卡片大小设置及按住 Ctrl 放大。 |
| 10-13 | [07baa3e](https://github.com/jimmgreen/pulse/commit/07baa3ea627105901b63c588466a09a6c2bdd9c3) | 悬停卡片内静音循环视频及播放进度细线；慢格式在触发等待期间显示行内进度，卡片内显示加载进度。 |
| 10-13 | [ee4521a](https://github.com/jimmgreen/pulse/commit/ee4521a47f1d025a1342594ab53b4cc76634f7f9) | 新增悬停卡片，显示预览、名称、类型及关键信息；可设置悬停或仅空格触发；卡片出现后空格打开快速预览。 |

当前本地未包含上述悬停卡片与增强包体系。若继续适配，建议优先处理预览取消与慢解码隔离，其次引入卡片交互，增强包更新另做独立改动。

## 文本比较来源

上游存在 `src/app/text_diff.*`、`src/ui/text_diff_window.*` 和 `src/app/tray_compare_metadata.*`，比较入口与暂存区关联。本地修改采用独立文本比较窗口和设置开关，通过文件右键选择比较源、再与第二个文件比较；不引入暂存区比较模式。
