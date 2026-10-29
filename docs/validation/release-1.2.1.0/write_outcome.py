"""Record release evidence for the existing combined report."""
import json
from pathlib import Path

here = Path(__file__).resolve().parent
load = lambda name: json.loads((here / name).read_text(encoding='utf-8-sig'))
validation = load('validation.json')
deployment = load('deployment-result.json')
installed = load('final-installed-verification.json')
artifacts = load('artifacts.json')
smoke = load('installed-smoke-result.json')
assert deployment['Success'] and deployment['BackupVerified']
assert installed['Success'] and installed['Version'] == '1.2.1.0'
assert artifacts['status'] == 'passed'
assert smoke['ExitCode'] == 0 and smoke['ScreenshotExists']
assert len(validation['DesktopCases']) == 6
assert all(case['accepted'] for case in validation['DesktopCases'])
lines = [
    '**当前状态：用户已手动认可持久合成原型，授权正式引入、升级为 1.2.1.0 并替换安装版。本轮已完成。**',
    '',
    '- 正式行为：移除生产代码的 `PULSE_TEST_COMPOSED_EDIT` 门控，透明合成宿主默认创建持久编辑表面；继续保留呈现不可用或失败时的原生回退。文字自动、清晰、平滑三选项保留。版本单一来源为 `version.txt=1.2.1.0`。',
    '- 定向验证：子编辑框自动／清晰／平滑各 153 PASS，真实高级搜索和批量重命名对话框各模式 425 PASS，文字设置 493 PASS，合计 2227 PASS / 0 FAIL。子编辑框检查含持续合成标记、键盘不切换原生 redraw、隐藏／显示、销毁、呈现失败后回退及 Unicode 编辑。',
    '- 保留失败证据：首次清晰模式只失败前台窗口保持断言，后续同样断言复测通过，未证明首次焦点变化根因。设置测试的低完整性构建副本无法准备或写入截图目录；使用 SHA256 完全一致的普通完整性副本后 493 项通过。测试入口补充标准异常报告，避免异常终止掩盖夹具错误。',
    '- 历史 GDI 像素夹具为 505 PASS / 48 FAIL，**没有计为通过，也没有删除或弱化断言**。其 `Pixels()` 用 `GetDC(edit)` + `BitBlt` 读取原生 EDIT 表面；新通道在其上方另有持久 DirectComposition 表面，底层即时 GDI 像素不能代表用户看见的文字。48 项集中于原生表面的松手／方向键字形断言，原日志见 `docs/validation/release-1.2.1.0/hosted.log`。',
    f'- 实际桌面复核：正式默认通道不设置原型环境变量，三模式 × 路径栏短文本／重命名长文本共 6 组、{int(validation["DesktopSamples"])} 个采样，均确认合成标记、可见字形，未采到第三种异常文字帧；查看了启动及编辑窗口截图。使用同步方向键消息，不能替代所有物理拖动、输入法、设备重建和跨屏 DPI 验证；用户本人对原型的手动确认单独记录，不包装成自动化结论。',
    f'- 发布核验：五个生产 EXE 的文件／产品版本均为 1.2.1.0；便携 ZIP 的 40 项载荷、文件名、哈希、许可证和无 selftest 检查通过。运行依赖检查要求静态 MSVC 运行库，新增必需 MSVC DLL 为 0。便携包 `dist/Pulse-1.2.1.0-portable-win-x64.zip`，SHA256 `{artifacts["portable"]["sha256"]}`；安装包 `dist/PulseSetup-1.2.1.0.exe`，SHA256 `{artifacts["installer"]["sha256"]}`。',
    f'- 安装替换：`D:\\Applications\\Pulse` 的 {deployment["BackupFiles"]} 个原文件完整备份并逐项哈希验证；仅替换 5 个变化的 EXE。回退目录 `{deployment["BackupDirectory"]}`。旧 UI 经原有托盘“退出”正常结束，未强杀；服务恢复原 Running / Auto、注册路径不变，索引 ACL 未改，原网络代理继续运行。',
    '- 原网络代理仍映射的旧 Index 映像暂留安装目录，具体路径见部署结果的 Deferred；没有为删除映像而结束代理。设置和索引目录未作迁移或清理。',
    '- 安装后生产程序实际启动、截图、正常退出 0；随后以普通 Medium 权限重新打开正式 UI。计划中的 7 个载荷哈希和版本匹配。发布采用备份后载荷替换，安装向导本轮没有运行；未声称执行完整历史回归。',
    '- 证据统一位于 `docs/validation/release-1.2.1.0/`：`validation.json`、`artifacts.json`、`deployment-result.json`、`final-installed-verification.json`、测试日志及截图。本节合并到现有唯一报告，不另生成一份完整报告。',
]
outcome = {
    'version': '1.2.1.0', 'status': 'released-and-installed',
    'default_composed_edits': True, 'validation': validation,
    'deployment': deployment, 'installed_verification': installed,
    'markdown_lines': lines,
}
(here / 'outcome.json').write_text(json.dumps(outcome, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print('Release outcome recorded; ready to merge into the existing report.')
