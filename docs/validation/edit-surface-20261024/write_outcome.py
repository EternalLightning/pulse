"""Append verified editor and deployment evidence to the single audit report."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LOGS = ROOT / 'bench_data/edit-surface-20261024'


def read(name):
    return json.loads((HERE / name).read_text(encoding='utf-8-sig'))


def link(path, label):
    return f'[{label}]({ROOT.as_posix()}/{path})'


tests = []
for label, expected in [('corrected', 553), ('desktop', 85), ('address-final', 11)]:
    content = (LOGS / f'{label}.log').read_text(encoding='utf-8-sig')
    exit_result = read(f'{label}-exit.json')
    assert exit_result['ExitCode'] == 0 and '[FAIL]' not in content
    assert content.count('[PASS]') == expected
    tests.append({'label': label, 'passed': expected, 'failed': 0,
                  'log': f'bench_data/edit-surface-20261024/{label}.log', 'exit': exit_result})

artifacts = read('artifacts.json')
plan = read('deployment-plan.json')
deployment = read('deployment-result.json')
installed = read('final-installed-verification.json')
smoke = read('installed-smoke-result.json')
preinstall = read('preinstall-smoke-result.json')
assert artifacts['status'] == 'passed'
assert deployment['Success'] and deployment['BackupVerified']
assert installed['Success'] and installed['Version'] == '1.2.0.3'
assert all(item['Match'] for item in installed['Files'])
assert smoke['Finished'] and smoke['ExitCode'] == 0 and smoke['ScreenshotExists']
assert smoke['SHA256'] == preinstall['SHA256']

lines = [
    '**当前状态：1.2.0.3 已完成本轮修复、定向验证、便携包和安装包；安装版已替换并以普通用户权限重新打开。** 1.2.0.2 的内部视口测试没有覆盖真实输入期间的中间画面，不能证明用户报告的闪烁已消除。',
    '',
    '- 确认的生产路径问题：原生 EDIT 在处理方向键时已经更新 GDI 画面，外层 LumaText 随后又绘制另一套字形/选区/光标；拖选期间冻结自绘滚动、松手后恢复跟随，会改变文字原点。鼠标选区活动端缓存也不能完整代表键盘反向扩选。路径、地址搜索、筛选、文件重命名、标签重命名共享该宿主路径。',
    '- 修复：以上五种宿主编辑框统一由 Windows EDIT 负责字体、绘制、光标、选区、命中、水平滚动和输入法消息；继续使用不透明的重定向子窗口表面，保证文字出现在 DirectComposition 主窗口上。删除此处的重复自绘、手动擦除和自绘颜色/字体转发辅助函数；保留实际业务快捷键、回车/Esc 行为。补齐 DPI/语言切换时筛选框的原生字体更新。全局搜索和菜单/对话框的独立编辑实现没有在本轮切换。',
    '',
    '| 定向验证 | 结果 | 实际覆盖 |',
    '|---|---:|---|',
    f'| 五种实际生产宿主编辑框 | 553 PASS / 0 FAIL | 深浅主题，100%/150%/200%，连续父窗口渲染，方向键中间画面、短文本字形稳定、长文本拖选/松手、真实活动光标、中文插入和撤销、保留标签快捷键。{link(tests[0]["log"], "日志")} |',
    f'| 真实桌面合成复核 | 85 PASS / 0 FAIL | 独立临时窗口中的路径栏和重命名栏，深浅主题、100%；从桌面取像素，原生表面与桌面一致，父窗口合成后可见，方向键只改变光标像素。包含重复基础断言，不叠加声称 85 个新场景。{link(tests[1]["log"], "日志")} |',
    f'| 实时搜索编辑 | 11 PASS / 0 FAIL | 连续查询更新、连续输入、文本中部插入、鼠标落点后的输入顺序。{link(tests[2]["log"], "日志")} |',
    '',
    '- 像素证据：五类 × 两主题 × 三缩放共 30 组，方向键的原生中间表面与最终表面差异均为 0 像素；拖选松手差异也均为 0 像素。短文本方向键最大变化分别为 38/56/74 像素，恰好是两个位置的一像素宽光标，未发现字形位置变化。',
    '- 测试诊断保留：原始修复前日志为 375 PASS / 58 FAIL；最初修复后为 521 PASS / 32 FAIL，未据此交付。逐帧位图证明其中 28 项来自截图抗锯齿颜色通道仅差 1/255；另 4 项集中在深浅主题的 200% 搜索框（各含短文本和拖选断言），测试仍固定为 1100 物理像素、输入区仅宽 48 像素，出现正常溢出滚动及越界拖选返回空选区，不适用“短文本字形不动”和最终选区必非空的前提。最终测试固定 1100 DIP，只忽略一色阶量化差异，保留光标/字形位移检测。没有删除失败日志或跳过断言；修改了测试条件，因此不将前后通过数视为完全相同测试集的对比。',
    '- 历史失败和逐帧证据：' + '、'.join([
        link('bench_data/edit-surface-20261024/before.log', '修复前'),
        link('bench_data/edit-surface-20261024/after.log', '首轮修复后'),
        link('bench_data/edit-surface-20261024/diagnostic.log', '量化与窄框诊断')]) + '。没有运行全部历史回归或完整 selftest；旧 rename selftest 同步改为检查原生光标和实际选区表面，本轮未独立运行该旧入口。中文字符插入验证不等于各输入法候选窗口均已验收。',
    '',
    f'- {link("dist/Pulse-1.2.0.3-portable-win-x64.zip", "1.2.0.3 便携包")}：SHA256 `{artifacts["portable"]["sha256"]}`。',
    f'- {link("dist/PulseSetup-1.2.0.3.exe", "1.2.0.3 安装包")}：SHA256 `{artifacts["installer"]["sha256"]}`。',
    '- 发布核验：40 项便携载荷逐项匹配，五个生产 EXE 均为 1.2.0.3，原始文件名、运行库和许可证检查通过。安装包已编译并记录文件身份，本轮通过保留状态的载荷替换更新现有安装，没有运行安装向导。' + link('docs/validation/edit-surface-20261024/artifacts.json', '发布证据') + '。',
    f'- 安装目录 `D:\\Applications\\Pulse`：只替换 {len(deployment["ChangedFiles"])} 个发生变化的 EXE；完整 {deployment["BackupFiles"]} 文件备份逐项验证，回退目录 `{plan["BackupDirectory"]}`。旧界面经正常托盘退出；没有强杀。设置、索引数据和索引目录权限保留，服务恢复原 Running / Auto，原网络代理继续运行。',
    '- 原网络代理仍映射的上一版 Index 映像暂留安装目录，未为清理该映像而结束代理；本轮不改索引策略。' + link('docs/validation/edit-surface-20261024/deployment-result.json', '完整部署与保留项记录') + '。',
    '- 安装后生产程序实际启动、截图、正常退出 0，随后重新打开普通 Medium 界面；7 项计划载荷哈希匹配。' + link('docs/validation/edit-surface-20261024/final-installed-verification.json', '安装后核验') + '、' + link('docs/validation/edit-surface-20261024/installed-smoke-result.json', '生产启动证据') + '。实际桌面定向检查使用同源生产宿主函数的隔离夹具，没有宣称已经完成用户本人手动验收或所有动画检查。',
]
result = {'status': 'installed-verified', 'accepted': True, 'version': '1.2.0.3',
          'tests': tests, 'artifacts': artifacts, 'deployment': deployment,
          'installed': installed, 'smoke': smoke, 'markdown_lines': lines}
(HERE / 'outcome.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'status': result['status'], 'tests': [item['passed'] for item in tests]}, ensure_ascii=False))
