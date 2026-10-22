# Pulse 1.2.0.0 便携包

- 文件：`dist/Pulse-1.2.0.0-portable-win-x64.zip`，12,376,743 字节，40 个文件。
- SHA256：`43AB14A5C5B757CD5482F2CEEDCE97BD3BAFDE511149F879734173B66C839F34`。
- 当前 Release 主程序及直接生产依赖增量构建通过，`PULSE_WITH_SELFTEST=OFF`；发布载荷检查通过。
- 五个程序的 FileVersion、ProductVersion 均为 `1.2.0.0`，OriginalFilename 正确。
- 40 个暂存文件、ZIP 条目和实际解压文件逐一哈希匹配；载荷与已验证的安装版动画发布一致。保留辅助程序、DLL、7zip、第三方许可证及说明，不含开发和测试文件。
- 检查九个 PE 文件的依赖，无外部 VCRUNTIME/MSVCP/CONCRT 导入；见 `runtime-dependencies.log`。
- 从实际解压目录以独立测试配置启动，成功生成 `portable-smoke.png` 并以退出码 0 退出；见 `portable-smoke.json`。
- 此包免安装，配置和缓存仍位于当前用户应用数据目录；解压不注册 PulseIndex 服务。
- 本轮未删除旧包、暂存目录、测试夹具或回滚备份。现有安装目录和服务不作更新。
- 本轮未重跑完整功能回归，也未进行人工动画验收、签名或杀毒扫描。

发布清单及各文件哈希见 `package-audit.json`，解压位置见 `extraction.json`；同目录保留本次打包及启动检查记录。
