# 无用代码审查与清理（2026-10-14）

本轮以现有未提交修改为起点，检查 app、ui、ops、fs、index 的低引用候选及相关调用链。以下仅记录本轮新增清理，不将此前资源删除、动画迁移等修改计入收益。

## 清理与证据

- `src/index/content_index.cpp`：`Impl::OtherQueryActive()` 全仓无调用，删除函数及其专用 `query_slots` 数组、构造和析构维护。每个实例不再为这条死路径创建四个命名互斥量句柄。实际查询使用的 `ReadSlot` 独立创建和持有 reader 互斥量，保持原样。
- `src/index/index_delta.h/.cpp`：`BaseBuilt()` 无调用，`base_built_` 仅在 Open 中赋值，删除访问器、字段和赋值。Open 参数仍用于磁盘头写入与校验，未改变格式或重放规则。
- `src/app/app_input.h`、`src/app/app_visual_animation.cpp`：四个动画辅助函数已仅在动画文件内使用，删除旧输入头文件声明，将定义收入匿名命名空间。动画实现保持原样。

合计五个源码文件，本轮删除 19 行、增加 3 行，净减 16 行。收益是减少死接口、冗余状态及句柄维护；未测量 EXE 大小、内存或性能净变化。

UI、文件操作和文件系统检查未找到足够可靠的新删除候选，未强行改动。兼容迁移、实际回退和测试接口不因引用少而删除。本次是定向死代码审查，不代表全仓不存在其他问题。

## 实际验证

- Release 构建 `pulse`、`pulse_index`、`pulse_pane_animation_application_test` 成功。
- 运行 `pulse_pane_animation_application_test.exe`：33 PASS，0 FAIL，退出码 0。
- 删除符号引用搜索无残留；本轮已跟踪文件 `git diff --check` 通过。
- 动画夹具成功生成四张截图；本轮未作人工视觉验收。

未运行完整自测试、内容索引集成套件或发布打包。索引清理依据引用检查、实际查询路径核对及构建验证，未宣称完成真实并发查询或跨进程运行验证。
