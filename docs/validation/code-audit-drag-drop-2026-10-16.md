# 拖放及公共存储补读 — 2026-10-16

## 拖放介质所有权修复

全文补读 `src/ui/drag_drop.cpp`、`src/ui/drag_drop.h`，并查看 app_main 的 OLE 注册/撤销与 app 调用位置。既有异步 Shell ID list 准备、拖放目标会话行为保留。

`ExtractHDropPaths` 原来把 GetData 失败和空 hGlobal 合并返回：GetData 成功但句柄为空时，未调用 ReleaseStgMedium，遗留 pUnkForRelease 引用。函数也未检查返回 tymed，直接把联合字段当作 HDROP。新增自有 IDataObject 夹具给出正确数据、空句柄和错误介质类型；自有释放对象追踪引用，并在旧实现验证后回收故意泄漏的测试引用。

修复后在 GetData 成功时立即建立作用域释放对象，在读取 HDROP 前拒绝非 TYMED_HGLOBAL 或空句柄。正常退出、拒绝和后续异常展开均释放介质。没有改变正常路径转换、拖放效果选择或增加 Shell/UI 线程工作。

实际验证：

- 旧生产实现配新 `--medium-only`：7 PASS、2 FAIL、退出 1。失败为缺少 owner 释放和错误介质未拒绝。
- 修复后 `pulse_drag_drop_test --medium-only`：9 PASS、0 FAIL、退出 0。
- `pulse_drag_drop_test --target-session-only`：8 PASS、0 FAIL、退出 0，覆盖最终对象重新读取、反馈刷新、离开和完成后的会话退休。
- 构建仅 `pulse_drag_drop_test`，MSVC 输出没有新增警告；主程序整合构建另记。没有运行完整拖放、Shell 兼容性或 selftest 套件。
- 新测试不使用真实剪贴板、不调用实际 Explorer 拖放，不修改用户数据。错误介质夹具故意以自有 owner 管理可安全释放的测试数据，并非实际第三方返回对象的端到端复现。

## 补读覆盖与尚未修复的边界

整合补记：主任务完成 `cmake --build build --target pulse`，退出 0，当前 drag_drop.cpp 已编译并链接进 Pulse.exe，没有新增编译警告。相关 diff 检查通过，仅换行转换提示。

新增全文阅读 `src/common/user_storage.cpp`、`src/common/user_storage.h`、`src/common/diagnostics_exporter.cpp`、`src/common/diagnostics_exporter.h`，未修改这些生产文件。全文阅读不等于功能验证。

- 存储迁移：CopyVerified 使用拒绝写入/删除共享的源与目标句柄核对字节；但失败回滚保存的是路径，之后按路径 DeleteFile，未保留目标对象身份到回滚。NoReparse 也是路径属性检查，没有目录租约覆盖迁移全程。存在需要独立并发夹具确认的替换窗口，本轮未动态复现、未宣称已解决。
- 诊断导出/清理：FindNextFile 循环退出未区分 ERROR_NO_MORE_FILES 与枚举 I/O 失败；个别目录/文件属性查询失败直接视作无内容。代码显示可能遗漏错误报告，尚无定向失败注入证据。本轮未改写导出策略，也未触碰真实崩溃日志。
- 拖放：本次修复介质类型与所有权，不宣称已穷尽外部 CF_HDROP 的损坏内存布局、极端大小、COM 重入或第三方阻塞行为。

UI 旧续篇中 19 个未全文文件，本轮补齐 drag_drop.cpp，仍有 18 个未全文；其他新增全文项属于 common。全项目审查仍未完成，实际 GUI、DPI、网络盘及第三方集成验收没有新增证据。
