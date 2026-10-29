"""Append this release's preserved evidence to the one audit report."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LOGS = ROOT / 'bench_data/drag-viewport-1.2.0.2'

def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig')) if path.exists() else None

artifacts = read(HERE / 'artifacts.json')
test_exit = read(HERE / 'hosted-edit-exit.json')
test_text = (LOGS / 'hosted-edit.log').read_text(encoding='utf-8-sig')
baseline = (ROOT / 'bench_data/quality-release-1.2.0.1/drag-viewport-scrolled-before.log').read_text(encoding='utf-8-sig')
assert artifacts['status'] == 'passed'
assert test_exit['ExitCode'] == 0 and '[FAIL]' not in test_text
passed = test_text.count('[PASS]')
baseline_failures = baseline.count('[FAIL]')
assert baseline_failures == 4
plan = read(HERE / 'deployment-plan.json')
deployment = read(HERE / 'deployment-result.json')
smoke = read(HERE / 'installed-smoke-result.json')
installed = read(HERE / 'final-installed-verification.json')
accepted = bool(deployment and deployment['Success'] and smoke and smoke['Finished'] and
                smoke['ExitCode'] == 0 and smoke['ScreenshotExists'] and installed and installed['Success'])
deployed = bool(deployment and deployment['Success'])
status = 'installed-verified' if accepted else 'installed-awaiting-verification' if deployed else 'packaged-awaiting-deployment'
lines = [
    '**当前状态：1.2.0.2 已完成修复、定向验证和两个发布包。' +
    ('安装文件、实际启动、普通用户界面和原服务状态均已核验。**' if accepted else
     '安装文件已替换，启动与恢复核验尚未完成。**' if deployed else
     '安装替换尚未完成；当前已安装版本仍为 1.2.0.1，正在等待 Windows 管理员启动。**'),
    '',
    '- 确认原因：长文本已经水平滚动时，鼠标命中会舍入到视口边缘之外；原先每次命中和重绘都跟随选区活动端调整水平偏移，造成下一次命中使用不同文字原点，快速往返拖选出现左右晃动。路径栏和重命名栏共用该编辑呈现实现。',
    '- 改动：捕获鼠标拖选期间保持框内视口固定；仅指针越出客户区时由输入事件调整滚动，重绘本身不推进滚动。文本修改和松开鼠标后的光标跟随继续工作。此前原生编辑与自绘表面交替造成的闪烁修复继续保留。',
    f'- 复现与回归：已滚动长文本在 100% / 150% 缩放、两种窗口呈现模式中，修正前出现 {baseline_failures} 项文字原点稳定性失败；修正后正式 pulse_child_edit_test 为 **{passed} PASS / 0 FAIL**。覆盖快速往返拖选、原生选区更新、左右越界滚动、无新输入的重复重绘、松开后光标移动和短文本重置。',
    '- [原失败日志](../bench_data/quality-release-1.2.0.1/drag-viewport-scrolled-before.log)、[修正后日志](../bench_data/drag-viewport-1.2.0.2/hosted-edit.log)、[退出记录](validation/drag-viewport-1.2.0.2/hosted-edit-exit.json)。这些是直接调用生产编辑控件与绘制路径的自动测试，未宣称完成真实鼠标手动验收或所有动画检查。',
    '- 从已有完整 Ninja Release 目录仅构建受影响编辑目标；版本资源统一后构建五个生产交付目标。未运行所有历史回归或完整自测。构建依赖与本地化编译修正沿用上一轮，未复用有空依赖的旧 NMake 对象。',
    '',
    f'- [1.2.0.2 便携包](../dist/Pulse-1.2.0.2-portable-win-x64.zip)：{artifacts["portable"]["size_bytes"]:,} B；SHA256 `{artifacts["portable"]["sha256"]}`。',
    f'- [1.2.0.2 安装包](../dist/PulseSetup-1.2.0.2.exe)：{artifacts["installer"]["size_bytes"]:,} B；SHA256 `{artifacts["installer"]["sha256"]}`。',
    '- [发布核验](validation/drag-viewport-1.2.0.2/artifacts.json)：40 项便携载荷逐项匹配，五个 EXE 版本与原始文件名一致，无测试和调试文件混入，运行库与许可证检查通过。安装包已编译并记录文件身份，未运行安装向导或单独解包核对。',
    '- [安装前生产启动](validation/drag-viewport-1.2.0.2/preinstall-smoke-result.json)：使用与构建文件哈希一致的普通 Medium 副本，实际启动、截图并正常退出 0；截图已查看。截图检查只证明启动和静态布局，不代替拖选动态测试。',
]
if accepted:
    lines += [
        f'- 安装目录 `D:\\Applications\\Pulse` 已更新为 **1.2.0.2**，只替换 {len(deployment["ChangedFiles"])} 项变化文件。完整 {deployment["BackupFiles"]} 文件回退备份逐项哈希验证，保存在 `{deployment["BackupDirectory"]}`。',
        '- 正常托盘退出旧 UI，未强杀用户程序；保留设置、索引数据和索引目录权限，PulseIndex 恢复原来的 Running / Auto 状态。所有计划载荷与安装文件哈希相同，随后以普通用户重新打开界面；[部署记录](validation/drag-viewport-1.2.0.2/deployment-result.json)、[安装后核验](validation/drag-viewport-1.2.0.2/final-installed-verification.json)、[安装后启动](validation/drag-viewport-1.2.0.2/installed-smoke-result.json)。',
        '- 安装目录中仍由原网络代理映射的历史旧映像保持原样；没有为清理备份而终止代理。此轮没有更改索引或搜索算法，也未重复运行索引性能测试。',
    ]
elif deployment:
    lines += ['- [部署记录](validation/drag-viewport-1.2.0.2/deployment-result.json)：最终安装验收尚未通过；不能视为交付完成。']
else:
    lines += [f'- 已准备完整备份与原子替换计划，目标回退目录 `{plan["BackupDirectory"]}`；Windows 管理员启动尚未完成，不将候选包生成等同于安装成功。']
outcome = dict(status=status, accepted=accepted, test_passed=passed, baseline_failures=baseline_failures,
               deployment=deployment, smoke=smoke, installed=installed, artifacts=artifacts, markdown_lines=lines)
(HERE / 'outcome.json').write_text(json.dumps(outcome, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(status=status, accepted=accepted, passed=passed), ensure_ascii=False))
