"""Summarize preserved validation and deployment evidence in the one audit report."""
from pathlib import Path
import json
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LOGS = ROOT / 'bench_data/quality-release-1.2.0.1'

def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

artifacts = read(HERE / 'artifacts.json')
assert artifacts['status'] == 'passed' and artifacts['portable']['payload_verified']
tests = []
names = {
    'hosted-edit': '正式编辑框 / 主题缩放',
    'address-edit': '正式地址栏持续输入',
    'removable-routing': '正式 USB IPC 搜索路由',
    'folder-sizes-ui': '正式目录尺寸缓存 / 排序',
    'removable-policy': '正式设备策略 / 挂载枚举',
    'index-cache': '生产模式候选缓存 / 名称池',
    'index-storage': '旧分卷与 aggregate 对照',
    'index-compaction': '压缩 / 损坏拒绝 / A/B 恢复',
}
for name, label in names.items():
    exit_result = read(LOGS / f'{name}-exit.json')
    text = (LOGS / f'{name}.log').read_text(encoding='utf-8-sig')
    assert exit_result['ExitCode'] == 0 and '[FAIL]' not in text, name
    tests.append(dict(name=name, label=label, passed=text.count('[PASS]'), failed=0, exit=exit_result))

deployment_path = HERE / 'deployment-result.json'
deployment = read(deployment_path) if deployment_path.exists() else None
rollback_path = HERE / 'rollback-after-smoke-failure/deployment-result.json'
rollback = read(rollback_path) if rollback_path.exists() else None
rolled_back = bool(rollback and rollback['Success'] and
                   (not deployment or rollback['Utc'] >= deployment['Utc']))
deployed = bool(deployment and deployment['Success'] and not rolled_back)
smoke_path = HERE / 'installed-smoke-result.json'
smoke = read(smoke_path) if smoke_path.exists() else None
restored_path = HERE / 'user-ui-restored.json'
restored = read(restored_path) if restored_path.exists() else None
installed_check_path = HERE / 'final-installed-verification.json'
installed_check = read(installed_check_path) if installed_check_path.exists() else None
accepted = bool(deployed and smoke and smoke['ExitCode'] == 0 and smoke['ScreenshotExists'] and
                restored and restored['Success'] and installed_check and installed_check['Success'])
status = 'installed-verified' if accepted else ('installed-awaiting-verification' if deployed else
    'rebuilding-after-smoke-failure' if rolled_back else 'verified-packaged-awaiting-deployment')
lines = [
    '**当前状态：已完成本轮代码质量改进、定向验证和两个 1.2.0.1 发布包。' +
    ('安装版文件、真实启动、用户界面和服务状态均已核验。完成日期：2026-10-24（Asia/Shanghai）。**' if accepted else
     '安装版文件已替换，实际启动验收尚未完成。**' if deployed else
     '首次安装启动检查失败后已回退旧版，正在修正依赖扫描并重新编译；候选包尚不可交付。**' if rolled_back else
     '安装替换尚未完成，等待 Windows 管理员启动确认；不得将候选包已生成等同于已安装。**'),
    '', '### 9 项审查问题的处理', '',
    '| 原项 | 改进结果 |', '|---|---|',
    '| 迟到 runas 破坏稳定菜单 | 仅初始快照可提升 COM runas；本次菜单中首次晚到项留动态区，下次打开再提升。冷菜单→部分/最终异步更新回归先复现 3 FAIL，再通过。 |',
    '| USB 目录挂载路由 | 后台枚举卷的全部挂载别名；已知 GUID 复用服务 kind/online，未知卷仅探测一次，UI 只读规范化缓存并按最长目录边界匹配。IPC 字节格式不变。 |',
    '| 常规测试接口滞后 | 补充主机测试直接协议依赖；旧离线测试按当前 Flatten/MFT builder 改写，保留 FRN、隐藏/排除、搜索、持久化、重启和恢复断言；未添加产品空实现。 |',
    '| 测试依赖临时 rsp | 编辑框、地址栏、USB 路由、尺寸缓存与设备策略都有正式 CMake 目标。四个应用回归共用一套生产对象；不再需要旧 bench_data 私有链接输入，移除路由测试中固定本机 DLL 路径。 |',
    '| 每帧全目录业务工作 | 请求按 snapshot/范围/详情路径失效；排序按面板生命周期、snapshot、方向、分组模式、尺寸修订失效。稳定帧不构造全量请求、不重复 Sync，也不重复排队/排序。保留 worker 容量恢复重试。 |',
    '| 导航平行栈叠加 | 六个平行容器收为前后两个 HistoryEntry 栈，路径、搜索来源、双轴视口在同一记录保存；删去长度不符时整栈丢元数据的补偿。旧路径记录仍可缺省字段。 |',
    '| 无效慢扩展策略 | 删除 deferred、慢成功计数及其触发的 UI 设置写盘；保留明确禁用和旧 JSON 自动禁用迁移，未恢复自动禁用。 |',
    '| 性能工具误导 | 明确样本数；单次显示 single，输出全部原始样本、min/median/max 与 nearest-rank p95；质量与时间预算分开，退出码仍由 enforce 合并验收。增加统计边界检查。 |',
    '| 旧优化残留 | 删除 index_changed 假指标及无效断言；旧分卷查询声明、字段、实现和调用仅在 storage 对照目标的宏内编译。生产只保留 aggregate；旧格式、WAL、回退和其他必要授权回调不删。 |',
    '', '### 正式目标实际验证', '',
    '| 检查 | 结果 | 日志 |', '|---|---:|---|',
]
for test in tests:
    lines.append(f'| {test["label"]} | {test["passed"]} PASS / 0 FAIL | [日志](../bench_data/quality-release-1.2.0.1/{test["name"]}.log) |')
lines += [
    '',
    '本轮子任务先前已完成：导航模型 69/0；冷菜单 runas 10/0；7-Zip/显式禁用兼容 10/0；离线保留 15/0；旧格式修复 44/0；主机目录尺寸协议 5/0；扫描 23/0、2 SKIP。前三项只保留在子任务工具输出，没有落盘日志，不伪造日志链接或为重复计数重跑；其余日志分别在 bench_data/quality-offline-retention.log、quality-legacy-repair.log、quality-host-folder-sizes-medium.log、folder-sizes-quality/scan-results.log。各测试范围有重叠，不相加宣称覆盖率。',
    '', '### 过程中发现的失败与修正', '',
    '- 用户看到的 pulse_folder_sizes_ui_cache_test.exe 弹窗是本轮新增排序缓存实现的真实空地址写入：vector::insert(end(), {}) 选中空 initializer_list 重载，返回 end 后被解引用。已改为 emplace_back 后取末项，保留原断言；此失败发生在安装替换前。',
    '- 随后双面板共享旧 snapshot 的转移优化造成 3 项计数断言失败。删除跨面板转移，统一在新 snapshot 身份改变后各重建一次；新增明确的一次失效断言，保留跨 750 ms 的 100 稳定帧强断言，最终 15/0。SetErrorMode 只防止测试崩溃反复弹框，不改变异常退出码或绕过检查。',
    '- 首次长构建中断；恢复后的受限环境链接器停在等待并留下部分 EXE。核对命令行后仅结束本轮构建树，在普通 Medium 环境重新完成。未强制结束用户 UI 或安装服务。',
    '- 新路由测试目标首次因没有继承 /MANIFEST:NO 发生重复 manifest 链接失败；正式质量目标统一继承生产链接选项，已重新构建和验证。',
    '- 首次实际安装的隔离截图启动发生 0xC0000005，立即从69文件校验备份回退旧版并重新打开，保留失败候选快照。生产对象166份依赖文件中99份为空；39个直接包含模型头的源中35个对象早于头文件且依赖为空，形成新旧模型布局混用。实测编译器只有2052中文资源，VSLANG=1033仍回退中文，不能假定实际输出英文。现在用UTF-8编译包装器与全新Ninja目录build_release_clean，拒绝复用本地化NMake依赖；发布脚本默认build-release并可指定目录。新app_navigation依赖记录302项，明确包含app_state.h/app_model.h。未用带测试宏的quality对象替换生产。',
    '- 所有早期日志保留；不把中断、环境失败、重复资源或首次崩溃说成通过。最终生产程序只有五个交付目标，PULSE_WITH_SELFTEST=OFF；没有默认运行完整旧自测或全部历史测试。',
    '- 全新构建EXE继承Low完整性标签，而截图目录为Medium，直接运行的截图写入返回1；未把它记为产品通过。将七项载荷按字节写入Medium隔离目录、逐项核对哈希后，生产启动与截图退出0，再进行最终部署。安装目录中的最终启动也退出0。未改用户全局ACL或用提权运行掩盖产品行为。',
    '', '### 发布包', '',
    f'- [便携包](../dist/Pulse-1.2.0.1-portable-win-x64.zip)：{artifacts["portable"]["size_bytes"]:,} B；SHA256 `{artifacts["portable"]["sha256"]}`。',
    f'- [安装包](../dist/PulseSetup-1.2.0.1.exe)：{artifacts["installer"]["size_bytes"]:,} B；SHA256 `{artifacts["installer"]["sha256"]}`。',
    '- [逐项核验](validation/quality-release-1.2.0.1/artifacts.json)：40 项便携载荷与 build/仓库资源大小和 SHA256 全匹配，五个 EXE FileVersion/ProductVersion/OriginalFilename 正确，无测试或调试文件混入，无内置自测，路径与许可证检查通过。安装包编译成功并记录身份，未单独解包比对或运行安装向导。',
    '', '### 安装与验证边界', '',
]
if deployed:
    lines += [
        f'- 完整备份并核验 {deployment["BackupFiles"]} 个原安装文件，恢复目录 `{deployment["BackupDirectory"]}`；实际替换 `{", ".join(deployment["ChangedFiles"])}`。其余原文件按备份哈希逐项核对。',
        '- 安装版采用正常托盘退出、管理员原位替换和恢复原 PulseIndex 启动/运行状态；没有运行会卸载旧版的安装向导，不改索引格式、索引位置或用户配置，不重建索引。',
        f'- [部署结果](validation/quality-release-1.2.0.1/deployment-result.json)，保留仍被旧进程占用的已知哈希映像：{len(deployment.get("Deferred") or [])} 项。',
    ]
    if smoke:
        lines.append(f'- 安装路径隔离截图启动：退出码 {smoke.get("ExitCode")}；结果见 [记录](validation/quality-release-1.2.0.1/installed-smoke-result.json)。静态截图不能替代物理拖选及动画连续帧验收。')
    if restored:
        lines.append('- 普通用户界面及网络代理恢复状态见 [记录](validation/quality-release-1.2.0.1/user-ui-restored.json)。')
    if deployment.get('Deferred'):
        lines.append('- 原网络代理保持原进程，不强制终止；它占用的一份旧Index映像按已知哈希保留。磁盘上五个正式程序均为1.2.0.1，服务已重启加载新程序；旧代理下一次正常退出后该退休映像才可回收。')
    if installed_check:
        lines.append('- [最终安装核验](validation/quality-release-1.2.0.1/final-installed-verification.json)记录五EXE版本、七项载荷SHA256、Running/Auto服务及持续运行主界面。')
else:
    lines.append('- 首次候选已经回退，原安装版1.2.0.0已重新打开；正在完成全新构建与启动验证。' if rolled_back else
                 '- 尚未取得成功部署记录；发布包核验通过不代表安装版已替换。Windows UAC 是服务程序替换所需的系统权限边界。')
lines += [
    '- USB 实际目录挂载/热插拔、真实 UAC 菜单命令与第三方菜单 Invoke、路径栏物理拖选未作为本轮运行验收。设备验证包括真实系统卷枚举与模拟别名经过生产 IPC 处理和路由。',
    '- 本轮改的是代码质量及验收口径，没有承诺把宽泛百万匹配查询压到 150 ms。安装后性能如仍超预算应按实际日志保留失败，不能以协议成功或单词毫秒级响应掩盖。',
]
search_path = HERE / 'installed-search-result.json'
search = read(search_path) if search_path.exists() else None
if search:
    raw = (LOGS / 'installed-search.log').read_text(encoding='utf-8-sig')
    samples = [line for line in raw.splitlines() if line.startswith('[SAMPLE]')]
    complete = len(samples) == 30 and all('error=0 incomplete=0' in line for line in samples)
    lines += ['', f'实际安装服务6词×5次共{len(samples)}次观测，全部完整且无协议错误：{complete}；质量与时间预算分开输出。enforce退出码{search["ExitCode"]}，宽泛a/ab的p95预算未通过，不能称速度全部达标。',
              '[实际搜索原始样本](../bench_data/quality-release-1.2.0.1/installed-search.log)。本轮a中位291.68ms、ab中位26.22ms，abc/report/pdf中位约6.05/2.08/2.05ms；五样本分位不代表长期p95。']
out = dict(version='1.2.0.1', status=status, tests=tests, artifacts=artifacts,
           deployment=deployment, rollback=rollback, smoke=smoke, restored=restored,
           installed_verification=installed_check, accepted=accepted, search=search, markdown_lines=lines)
(HERE / 'outcome.json').write_text(json.dumps(out, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(status=status, formal_tests=len(tests), deployed=deployed)))
