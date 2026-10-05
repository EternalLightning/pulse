# 保持用户可观察行为的架构优化

日期：2026-10-05。基于 [接口与架构审阅](E:/Projects/pulse/docs/architecture-interface-review.md)，在源码修改前记录本次边界。保留工作区已有未提交修改。

## 审阅结论与实施边界

| 原审阅项 | 本次处理 | 必须保持的行为 |
|---|---|---|
| F2 搜索合并复制 | 合并参数改只读引用，排序临时引用，只复制分页最终选中的 Hit | 候选顺序、原比较器、完整排序、total/结果元数据、offset/limit 规则、同键项顺序和 provider 快照不变；不修改翻页请求或订阅 |
| F4 菜单重复 DTO | 一个共享菜单项契约，既有类型名用 alias；直接移动原 vector | 字段默认值、菜单项顺序、partial/slow_clsids、token 映射、会话生命周期和 UI 呈现转换不变；wire 格式不变 |
| F5 UI 编译网络后端 | 将唯一合并实现移到独立源文件，生产 UI 排除网络执行体 | 网络 agent 仍负责执行；自测确需执行体时保留；不改变进程、连接、超时或配置 |
| F8 排序头文件依赖 | 排序枚举放入轻量头，worker/排序只包含所需契约 | 枚举名称、命名空间、底层值和比较行为不变 |

这些修改不新增执行后端、接口继承层或通用服务。F1 的列表/详情扫描完整性不同，F3 的文本容错不同，F6/F7 涉及兼容与传输策略，F9 涉及原始卷标所有权，本次不改变这些语义。F2 的稳定游标、双路有界合并、预览映射生命周期同样不在本次范围。

## 验证计划

搜索使用旧合并实现作为输出基准，对排序、排名、升降序、同键项、空输入、offset/limit、来源更新逐字段比较，并验证输入快照保持不变。菜单复用现有会话定向检查；纯枚举搬迁不机械增加测试，构建直接依赖目标并运行已有排序/worker 相关检查。只运行受影响目标和筛选，不运行完整历史套件，不改用户数据或偏好。不将编译通过或源码减少复制等同于性能实测。

## 实际实施与验证

### 已实施

- [search_result_merge.cpp](E:/Projects/pulse/src/index/search_result_merge.cpp) 是唯一生产合并实现。输入改为只读引用；临时 vector 保存 `const Hit*`，按原比较器和原 `std::sort` 完整排序，随后复制最终页。候选初始顺序仍为 local 后 network；保留 total 相加、默认结果元数据和原分页计算，不更改错误/不完整状态传播语义。provider 快照保留供下一次订阅更新使用，函数返回的页面拥有独立字符串。
- [network_index.cpp](E:/Projects/pulse/src/index/network_index.cpp) 移除原合并函数及其专用比较器。[CMakeLists.txt](E:/Projects/pulse/CMakeLists.txt) 在 `PULSE_WITH_SELFTEST=OFF` 的生产 UI 排除网络执行源；生成的 `pulse.dir/build.make` 仅列合并源，`pulse_index.dir/build.make` 仍列网络执行源。旧自测配置可保留网络执行体；未启用或运行完整自测。
- [shell_menu_item.h](E:/Projects/pulse/src/common/shell_menu_item.h) 保存唯一菜单项结构，`ipc::CtxMenuItem` 和 `ops::ShellMenuItem` 为 alias。[ops_manager.cpp](E:/Projects/pulse/src/ops/ops_manager.cpp) 将客户端原 vector 直接移动给 `OnCtxItems`。字段/默认值逐项核对一致，协议序列化代码和 token 映射没有改变。
- [sort_types.h](E:/Projects/pulse/src/ui/sort_types.h) 保存原排序枚举，名称、`pulse::ui` 命名空间、隐式 int 类型及值逐字保留。worker/entry_sort 直接包含轻量头，worker 显式包含原本间接获得的 `<memory>`；没有修改排序比较器或加载逻辑。

### 定向验证

| 检查 | 实际结果 |
|---|---|
| `pulse_search_merge_test` | 17 项断言通过，对照原值传递实现的 16,128 组查询/页组合，0 差异。覆盖 0/1/2/31/32/33/257 项、四种排序、排名/非排名、升降序、空/中英文/拼音查询、大小写等键、长/UNC 路径、空页及越界 offset；额外检查来源更新、输入别名和临时输入销毁后的结果所有权。 |
| `pulse_app_controllers_test --context-menu-com` | 复用原有 10 项断言，全部通过：partial/final、过期 token、静态与 COM 行组合、live ID 重映射、调用 verb/text 与刷新期限。筛选使用 `persist=false` 的内存偏好和模拟 Shell 回调，未启动真实 Shell。默认套件仍调用同一测试块。 |
| `pulse_worker_lifecycle_test` | 4 项通过：任务启动、停止有界、停止后拒绝请求、无晚到 UI 完成。 |
| `pulse_known_folder_labels_test` | 31 项通过：中英文目录排序、大小写等键、实际/虚拟路径及历史名称。只读检查，没有修改用户文件或偏好。 |
| 定向测试构建 | 上述四目标和现有 `pulse_index_test` 构建成功；后者仅验证搬迁后的链接，未运行其默认混合套件。 |
| 生产目标构建 | `pulse`、`pulse_index`、`pulse_shell` 全部构建成功，未出现新增编译警告。 |

worker 与目录标签测试在沙箱启动时均返回 `0xC0000142`，未运行断言；相同程序经自动审批在完整权限环境通过。第一次 worker 审批请求超时，按审批提示重试一次后成功。没有把启动错误判为本次功能回归。

### 结论边界

本次没有视觉布局、文案、快捷键、交互动作或进程协议变更；没有启动实际索引服务或第三方 Shell 扩展。定向模型与旧实现对照证明已覆盖场景的结果等价，不声称覆盖全部运行环境。排序仍是完整排序，分页仍重取前缀，复杂度仍为 O(m log m)；仅减少候选字符串复制、菜单 vector 搬运和编译依赖，未测得实际时延或内存峰值。Query/Hit 等契约仍在原 Engine 头，network client 仍包含原 NetworkIndex 头；本次只排除 UI 的执行源编译，未把所有后端声明依赖一并拆走。目录扫描和预览解码等存在行为差异的整改继续保留为后续审阅项。
