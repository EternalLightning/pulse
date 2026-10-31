# Third-party notices / 第三方声明

Pulse 自身的代码、脚本和文档采用根目录的 [Apache-2.0](LICENSE)。以下组件和素材保留各自许可；本文件是索引，不替代许可证全文，也不将第三方作品重新许可为 Apache-2.0。

| 组件或素材 | 范围及许可证 | 仓库中的完整声明 | 发行包位置 |
| --- | --- | --- | --- |
| ib-matcher 拼音数据 | MIT；包括生成的 `ascii_data.inc` | [许可证](third_party/ib-pinyin-cpp/LICENSE.txt)、[来源及修改](third_party/ib-pinyin-cpp/README.pulse.txt) | `licenses/ib-pinyin/` |
| SQLite | 公有领域；原始源码中的声明保留 | [来源及公有领域说明](third_party/sqlite/README.md)；`sqlite3.c`、`sqlite3.h` 文件头 | `licenses/SQLite/README.md` |
| PDFium 及其依赖 | 多种许可证；须保留完整许可证树，不能只保留二进制分发者的 MIT 许可证 | [分发许可证](third_party/pdfium/LICENSE)、[完整许可证目录](third_party/pdfium/licenses)、[来源](third_party/pdfium/README.pulse.md) | `licenses/PDFium/` |
| LumaText 及其依赖 | MIT，以及 FreeType、HarfBuzz、Unicode 等各自条款 | [完整许可证目录](third_party/lumatext/share/LumaText/licenses) | `licenses/LumaText/` |
| 7-Zip | LGPL-2.1-or-later，部分代码另含 BSD 和 unRAR 限制 | [许可证及版权](third_party/7zip/License.txt)、[来源](third_party/7zip/README.md) | `7zip/` |
| Files 应用图标 | MIT；`src/app/pulse.ico`、`src/app/pulse-icon.png`、`assets/global-search-logo.png` | [许可证](third_party/files-icon/LICENSE-MIT.txt)、[来源](third_party/files-icon/README.md) | `licenses/FilesIcon/` |
| Files 工具栏图标 | MIT；`assets/files_toolbar/` | [许可证](assets/files_toolbar/LICENSE-MIT) | `licenses/FilesToolbar/LICENSE-MIT` |
| Fluent System Icons | MIT；`assets/fluent/` | [许可证](assets/fluent/LICENSE)、[来源](assets/fluent/SOURCES.md) | `licenses/FluentSystemIcons/` |
| Microsoft OneDrive 产品图标 | Microsoft Fabric Assets License Agreement；不属于 Fluent System Icons 的 MIT 授权范围 | [协议原文](assets/microsoft_brand/LICENSE.pdf)、[使用范围及来源](assets/microsoft_brand/README.md) | `licenses/MicrosoftBrand/` |
| d2d-mica 材质图适配 | MIT；`src/ui/window_material.cpp` 中的自定义背景材质图；Copyright (C) 2022 by wangwenx190 (Yuhang Zhao) | [许可证](third_party/d2d-mica/LICENSE)、[来源及修改](third_party/d2d-mica/README.pulse.md) | `licenses/d2d-mica/` |

7-Zip 是单独调用的程序，不能将其二进制按 Apache-2.0 再许可。发行包在 `7zip/7z2603-src.7z` 中附带官方 26.03 对应源码，并包含 LGPL 2.1、GPL 2 全文及 BSD、unRAR 声明；unRAR 限制禁止将相关代码用于开发 RAR 压缩器。版本、校验值及源码获取信息见其 README。更新任何依赖时，应同步更新对应源码、许可证和本索引。

商标和产品名称属于各自权利人；此处列明来源不表示其对 Pulse 的认可或关联关系。本清单覆盖已识别的随程序分发的组件；构建工具和测试夹具若另行分发，也应保留其原有声明。
