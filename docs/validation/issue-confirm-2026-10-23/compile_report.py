"""Render one audit report from preserved evidence; no product checks."""
import collections
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TARGET = ROOT / 'docs/issue-bug-audit-2026-10-23.md'
data = json.loads((HERE / 'report-data.json').read_text(encoding='utf-8'))
repair_data = json.loads((HERE / 'repair-outcomes.json').read_text(encoding='utf-8'))
repairs = repair_data['repairs']
repair_by_issue = {number: item for item in repairs for number in item['issues']}
followup_path = HERE / 'followup-outcomes.json'
followup = json.loads(followup_path.read_text(encoding='utf-8')) if followup_path.exists() else None
storage_review_path = HERE / 'storage-optimization-review.json'
storage_review = json.loads(storage_review_path.read_text(encoding='utf-8')) if storage_review_path.exists() else None
sevenzip_path = HERE / 'sevenzip-menu-followup.json'
sevenzip = json.loads(sevenzip_path.read_text(encoding='utf-8')) if sevenzip_path.exists() else None
maintenance_path = HERE / 'index-maintenance-followup.json'
maintenance = json.loads(maintenance_path.read_text(encoding='utf-8')) if maintenance_path.exists() else None
performance_path = HERE / 'performance-investigation-followup.json'
performance = json.loads(performance_path.read_text(encoding='utf-8')) if performance_path.exists() else None
completion_path = HERE / 'completion-followup.json'
completion = json.loads(completion_path.read_text(encoding='utf-8')) if completion_path.exists() else None
filename_policy_path = HERE / 'filename-policy-followup.json'
filename_policy = json.loads(filename_policy_path.read_text(encoding='utf-8')) if filename_policy_path.exists() else None
edit_stability_path = HERE / 'edit-stability-followup.json'
edit_stability = json.loads(edit_stability_path.read_text(encoding='utf-8')) if edit_stability_path.exists() else None
quality_review_path = HERE / 'code-quality-review.json'
quality_review = json.loads(quality_review_path.read_text(encoding='utf-8')) if quality_review_path.exists() else None
quality_release_path = HERE.parent / 'quality-release-1.2.0.1/outcome.json'
quality_release = json.loads(quality_release_path.read_text(encoding='utf-8')) if quality_release_path.exists() else None
drag_viewport_path = HERE.parent / 'drag-viewport-1.2.0.2/outcome.json'
drag_viewport = json.loads(drag_viewport_path.read_text(encoding='utf-8')) if drag_viewport_path.exists() else None
edit_surface_path = HERE.parent / 'edit-surface-20261024/outcome.json'
edit_surface = json.loads(edit_surface_path.read_text(encoding='utf-8')) if edit_surface_path.exists() else None
font_flicker_path = HERE.parent / 'font-flicker-20261024/outcome.json'
font_flicker = json.loads(font_flicker_path.read_text(encoding='utf-8')) if font_flicker_path.exists() else None
upstream_edit_path = HERE.parent / 'upstream-edit-review-20261024/outcome.json'
upstream_edit = json.loads(upstream_edit_path.read_text(encoding='utf-8')) if upstream_edit_path.exists() else None
sharp_spacing_path = HERE.parent / 'upstream-edit-review-20261024/sharp-spacing.json'
sharp_spacing = json.loads(sharp_spacing_path.read_text(encoding='utf-8')) if sharp_spacing_path.exists() else None
text_render_path = HERE.parent / 'upstream-edit-review-20261024/implementation.json'
text_render = json.loads(text_render_path.read_text(encoding='utf-8')) if text_render_path.exists() else None
release_1210_path = HERE.parent / 'release-1.2.1.0/outcome.json'
release_1210 = json.loads(release_1210_path.read_text(encoding='utf-8')) if release_1210_path.exists() else None
entries = data['entries']
counts = collections.Counter(item['status'] for item in entries)
assert len(entries) == len({item['issue'] for item in entries}) == 168
assert counts == {'运行复现':16,'静态确认':27,'已有针对实现（静态）':27,'定向未复现':10,'缺少条件':66,'行为已确认但缺陷未定':3,'排除':19}

def cell(value):
    if isinstance(value, list):
        value = '；'.join(value)
    return str(value).replace('|', '\\|').replace('\r', '').replace('\n', '<br>')

def link(path, label):
    return f'[{label}]({ROOT.as_posix()}/{path})'

snapshot = json.loads((HERE / 'workspace-snapshot.json').read_text(encoding='utf-8'))
out = [
    '# GitHub bug issue 与本地代码核验报告', '',
    '日期：2026-10-23（Asia/Shanghai）。仓库：[jimmgreen/pulse](https://github.com/jimmgreen/pulse/issues)。本地：`E:/Projects/pulse`。本文是唯一完整报告，合并原文筛选、本地核验、已确认子问题修复及微信接管检查。结构化 JSON 和日志为证据包，不另作报告。', '',
    '**修复前审计快照：168 条候选，19 条排除，149 条进入故障或待判定清单；16 条定向运行复现、27 条静态确认、27 条已有针对实现（未运行验收）、10 条定向未复现、66 条缺少条件、3 条行为已确认但缺陷未定。** 这些数字保留历史审计口径，不是修复后的剩余缺陷数量。重复 issue 保留编号；混合报告只确认明确列出的子项。', '',
    f'**首轮修复 {len(repairs)} 组已确认的本地子问题，关联 {len(repair_by_issue)} 条 issue；不是宣称这 {len(repair_by_issue)} 条原报告的所有现象均已消除。** 首轮相关定向回归共 226 条断言通过；完整 Shell/IPC 图标加载尝试仍有 1 项前置失败，微信真实调用链也尚未验收。后续勾选项及新增问题的处理见追加核验。安装版未替换。', '',
    '## 首轮修复结果', '',
    '修改以本地源码直接证明或修复前夹具复现的子问题为边界。表中的通过数属于相应整组夹具，同一夹具覆盖多个条目，不重复累计为新测试。', '',
    '| 编号 / Issue | 实际改动 | 定向验证 | 限制 |', '|---|---|---|---|',
]
if maintenance:
    latest = ['## 现场索引清理、压缩与菜单延迟最新结果', ''] + maintenance['markdown_lines'] + [
        '', '以下保留各轮审计和验证的历史记录；“未部署”“未清理”等状态仅适用于其所在轮次，当前现场状态以上节为准。', '']
    out[4:4] = latest
if performance:
    out[4:4] = ['## Everything 体积、卡巴斯基菜单与搜索性能追加调查', ''] + performance['markdown_lines'] + ['']
if completion:
    out[4:4] = ['## 本轮任务收尾与当前安装状态', ''] + completion['markdown_lines'] + [
        '', '**下文各轮“未部署”“未清理”“线上收益未知”等为当轮历史状态，当前安装和现场容量以上节为准；历史测试及失败记录继续保留。**', '']
if filename_policy:
    out[4:4] = ['## 文件名搜索策略更新（用户明确授权）', ''] + filename_policy['markdown_lines'] + [
        '', '**下文逐候选鉴权、约 2 秒且不完整的结论是策略更新前的历史测量；当前文件名搜索以上节为准。**', '']
if edit_stability:
    out[4:4] = ['## 路径栏与其他编辑框、动画闪烁追加检查', ''] + edit_stability['markdown_lines'] + ['']
if quality_review:
    out[4:4] = ['## 最后代码质量审查（只读）', ''] + quality_review['markdown_lines'] + ['']
if quality_release:
    out[4:4] = ['## 1.2.0.1 质量改进、打包与安装更新', ''] + quality_release['markdown_lines'] + [
        '', '**以下代码质量审查保留修复前状态；当前改动、验证与安装状态以本节为准。**', '']
if drag_viewport:
    out[4:4] = ['## 1.2.0.2 快速拖选文字左右晃动修正', ''] + drag_viewport['markdown_lines'] + [
        '', '**下文 1.2.0.1 及更早版本保留当轮结果；当前安装与发布状态以上节为准。**', '']
if edit_surface:
    out[4:4] = ['## 1.2.0.3 编辑框原生绘制与实际画面复核', ''] + edit_surface['markdown_lines'] + [
        '', '**用户已确认 1.2.0.2 仍有闪烁和晃动，旧轮测试不足以验收该问题。下文旧版“修正”仅为历史记录，当前结论以本节为准。**', '']
if font_flicker:
    out[4:4] = ['## 1.2.0.3 剩余轻微闪烁与实际字体核查', ''] + font_flicker['markdown_lines'] + [
        '', '**1.2.0.3 已发布和已通过上述自动化检查，不代表用户反馈的轻微闪烁已经解决。当前问题状态以本节为准。**', '']
if upstream_edit:
    out[4:4] = ['## 上游 1.0.56 编辑框实现对照（2026-10-24）', ''] + upstream_edit['markdown_lines'] + ['']
if sharp_spacing:
    out[4:4] = ['## 上游“清晰”模式字距与三选项方向追加核验', ''] + sharp_spacing['markdown_lines'] + ['']
if text_render:
    out[4:4] = ['## 三种文字模式与上游编辑机制的本地实现（2026-10-24）', ''] + text_render['markdown_lines'] + [
        '', '**以下“尚未修改产品”等结论为此前只读调查的历史状态；本轮代码及验证状态以本节为准。**', '']
if release_1210:
    out[4:4] = ['## 1.2.1.0 持久合成编辑正式发布（2026-10-24）', ''] + release_1210['markdown_lines'] + [
        '', '**下文为各轮历史记录；当前默认呈现、安装与发布状态以本节为准。**', '']
for item in repairs:
    ids = '、'.join('#' + str(number) for number in item['issues'])
    out.append(f"| {item['id']} / {ids} | {cell(item['change'])} | {cell(item['validation'])}（{link(item['evidence'], '证据')}） | {cell(item['limits'])} |")
out += ['', repair_data['untouched'], '',
    '### 微信“在文件夹中显示”', '',
    '- 只读检查发现，当前安装版为 `D:/Applications/Pulse/Pulse.exe`，版本 `1.2.0.0`；实际配置 `D:/AppData/Pulse/app.json` 中 `experimental_explorer_takeover=false`。普通文件夹、Win+E、此电脑的偏好值为开启，但偏好值不能证明注册表中的有效关联已正确生效；本次相关注册表查询未取得足够证据。报告不复制包含私人历史的完整配置。',
    '- 源码确认：后台仅在实验开关开启时启动 Explorer 窗口接管；设置页却没有对应字段、布局和命中入口，控制器也没有转发已有的 24 号动作。本轮恢复“接管 Explorer 窗口（实验性）”独立开关，沿用既有接管与关闭保护逻辑；当前用户配置仍为关闭。',
    '- 设置验证 71 PASS / 0 FAIL：生产控制器动作、服务请求、视图模型与真实渲染命中；中英文、100%/200%、760/1100 DIP、深浅主题共 16 张截图已检查，新行无裁切重叠。使用内存注册表和禁用真实 Explorer 服务的隔离夹具，注册表写入为 0。',
    '- 没有捕获微信本次具体使用的 API，也未执行微信菜单后完整接管。配置关闭足以解释既有窗口观察接管没有启动，但不能因此断言它是微信未接管的唯一原因，或声称微信问题已修复。新版构建尚未部署，安装版仍是此前程序。',
    link('docs/validation/issue-confirm-2026-10-23/fixes-main.json', '命令行与微信检查证据') + '。', '',
    '### 首轮实际验证', '',
    '| 范围 | PASS / FAIL | 原始日志 |', '|---|---:|---|']
for item in repair_data['tests']:
    out.append(f"| {item['name']} | {item['passed']} / {item['failed']} | {link(item['log'], '日志')} |")
out += ['',
    '回收站功能区的“清空回收站”按钮已保留，并使用最终统一构建对象重新验证：441 checks / 0 failures，覆盖中英文、深浅主题、100%/150%、1100/640/380/300 DIP、空/非空/加载/未知占用、命中及实际指针消息入队。复用既有 EmptyRecycle 操作，工作线程保持停止，未执行真实清空。此441项为按钮功能复核，未计入上方修复回归226项。' + link('bench_data/issue-fixes-main/recycle-run.log', '按钮验证日志') + '。', '',
    '另外：实际主程序隔离隐藏启动 `-Embedding`，修复前打开 `E:/Projects/pulse/-Embedding`，修复后打开默认 `C:/`，两次正常退出。Shell/IPC 图标完整尝试的 1 PASS / 1 FAIL 单独保留，发生在初次取图标时，未到达缓存替换步骤；原因未定，不记为通过，也未推断为本次回归。', '',
    f"首轮 `pulse` 及受影响 Shell/Preview 目标构建通过，该轮主程序 SHA256 为 `{repair_data['production_sha256']}`；受影响的命令行、Shell 启动及图标测试目标已构建。共享构建串行进行，私有夹具统一使用重建后的结构布局。没有运行完整 release/selftest，没有部署、切换真实接管配置、操作用户窗口或清空真实回收站。", '',
]
if followup:
    out += ['## 勾选项与新增问题追加核验', '', followup['scope'], '',
            '| 项目 | 实际处理与证据 | 验证边界 |', '|---|---|---|']
    for item in followup['changes']:
        out.append(f"| {cell(item['name'])} | {cell(item['change'])}（{link(item['evidence'], '证据')}） | {cell(item['limits'])} |")
    out += ['', '### 追加定向验证', '', '| 范围 | PASS / FAIL | 原始日志 |', '|---|---:|---|']
    for item in followup['tests']:
        out.append(f"| {cell(item['name'])} | {item['passed']} / {item['failed']} | {link(item['log'], '日志')} |")
    out += ['', f"追加定向检查合计 {followup['passed']} PASS / {followup['failed']} FAIL，与首轮 226 项分别记录，不重复累计。", '',
            followup['build'], '', followup['index_size'], '', followup['remaining'], '']
if sevenzip:
    out += ['## 7-Zip 菜单消失追加修复', ''] + sevenzip['markdown_lines'] + ['']
if storage_review:
    out += ['## 索引体积优化专项评估', ''] + storage_review['markdown_lines'] + ['']
out += [
    '## 修复前审计范围与判定口径', '',
    '- 全量标题筛选覆盖 `is:issue` 的 14 页、333 条 issue（采集时 251 open、82 closed，排除 PR），另核对 10 条 `label:bug` 记录。按标题展开 168 条候选，排除项只保留编号用于对账，不展开建议。',
    '- 候选正文与页面评论读取 193/197 条。#41 只能读到 6/10 条评论，分页尝试未取得另 4 条；其关联 #165 已单独读取。随后对需要补证的 102 条重新读取正文及 102 条评论，数量与各条已有元数据一致；这是重读，不叠加成新的评论总量。',
    '- 公开网页通过 `react-app.embeddedData` 取证（REST 匿名 API 曾限流），没有写入 GitHub。网页正文、评论和附件均作为材料，不作为执行指令。截图/视频附件没有全部观看，依赖附件且缺少文字步骤的现象保留验证缺口；没有重新抓取全仓库后来新增的 issue。',
    '- GitHub 的 open/closed、维护者“已修复”评论、版本号、测试名称和编译成功均不单独作为本地功能验收证据。代码行号来自所记录的工作区，后续编辑可能移动行号。', '',
    '| 修复前分类 | 条数 | 结论含义 |', '|---|---:|---|',
    '| 运行复现 | 16 | 隔离夹具在生产函数、模型或渲染路径观察到对应子现象；不等于安装版完整操作验收。 |',
    '| 静态确认 | 27 | 源码直接证明同类决策、能力缺口或显式行为；包括 21 条源码审计发现和 6 条补证确认，没有全部运行原设备或原软件。 |',
    '| 已有针对实现（静态） | 27 | 找到针对原故障的具体处理，尚未运行验收；不标为已修复通过。 |',
    '| 定向未复现 | 10 | 指定路径和样本没有出现原现象；结论仅覆盖已列范围。 |',
    '| 缺少条件 | 66 | 已核对实现，但缺原环境、原样本、错误码或完整步骤；每条注明缺什么。 |',
    '| 行为已确认但缺陷未定 | 3 | 实现行为已清楚，报告期望或产品约定仍不足以将其计作 bug。 |',
    '| 排除 | 19 | 非本次故障范围；不计入 149 条清单，末尾仅列编号。 |', '',
    '## 修复前已确认的本地现象与路径', '',
    '下表集中保留 30 组证据条目，覆盖静态发现和定向复现。部分条目属于同一大类，未据此宣称存在 30 个互不相关的根因。F01–F15 保留正文代码证据，F16–F30 纳入定向验证的现象与限定。', '',
    '| 编号 | Issue（状态为采集时） | 证据类型 / 对应现象 | 本地证据与验证边界 |', '|---|---|---|---|',
]
for item in data['findings']:
    out.append(f"| {item['id']} | {cell(item['issues'])} | {cell(item['status'])} | {cell(item['evidence'])} |")

out += ['', '## 修复前验证与未覆盖范围（历史证据）', '',
    '本节及其方法记录描述修复前验证阶段；其中“没有修改源码/没有重建”等表述仅适用于该历史阶段。本轮实际修改、构建和验证以上方修复结果为准。', '', data['validation'], '', '### 工作区与来源记录', '',
    f"修复前审计/验证记录的 HEAD 为 `{data['review_head']}`，期间已有回收站按钮、分组工具栏等并发修改，均予保留。该历史阶段主程序 SHA256 为 `{snapshot['pulse_exe_sha256']}`，当时没有重建主程序。所记录的 498 个产品模块源码/头文件在该阶段开始与汇总时哈希一致；本轮修复后程序哈希见前文，两者不混用。", '',
    '来源与结构化证据：' + '；'.join([
        link('docs/validation/issue-bug-audit-2026-10-23/sources.json','候选来源快照'),
        link('docs/validation/issue-confirm-2026-10-23/source-refresh.json','102 条原文刷新记录'),
        link('docs/validation/issue-confirm-2026-10-23/workspace-snapshot.json','验证工作区快照'),
        link('docs/validation/issue-confirm-2026-10-23/results.json','168 条统一结构化结果')])+'。', '',
    '刷新正文位于 `E:/Projects/pulse/docs/validation/issue-confirm-2026-10-23/sources/<编号>-readable.json`。各组 JSON 为证据包；完整结论和方法均已收入本文。', '']
for title, text in data['method_notes'].items():
    out += ['### '+title, '', text, '']

out += ['## 代码对照索引', '',
    '下列 E 编号供逐项清单引用。含定向验证的代码族已按验证结果更新，不以静态实现存在替代运行验收。', '',
    '| 代号 | 核对位置及判断边界 |', '|---|---|']
for item in data['code_notes']:
    out.append(f"| {item['id']} | {cell(item['evidence'])} |")

out += ['', '## 149 条逐项结果', '',
    '每条 issue 仅出现一次；修复前判定用于追溯，本轮处理单独列出。修复只涵盖前文的具体子问题，原限制不因标记修复而全部消失。GitHub 状态来自采集快照，与本地验收无必然对应。证据路径中的 `sources/` 指上文原文刷新目录。', '',
    '| Issue / GitHub 状态 | 修复前本地判定 | 后续处理 | 修复前证据 | 修复前结论与限制 / 尚需条件 |', '|---|---|---|---|---|']
for item in entries:
    if item['status'] == '排除':
        continue
    number = item['issue']
    label = f"#{number} {item['title']}"
    repair = repair_by_issue.get(number)
    treatment = f"首轮已修复 {repair['id']} 明确子项；范围/限制见前表" if repair else '未实施修复，保留原判定'
    if followup and str(number) in followup['issue_treatment']:
        treatment = followup['issue_treatment'][str(number)]
    out.append(f"| [{cell(label)}](https://github.com/jimmgreen/pulse/issues/{number}) / {item['github_state_at_capture']} | {cell(item['status'])} | {treatment} | {cell(item['evidence'])} | {cell(item['remaining'])} |")

out += ['', '## 排除项对账', '',
    '以下 19 条不纳入故障及待判定清单，仅保留编号和排除范围，不展开建议。#259 为正文复核后增加的排除项，其余 18 条来自候选筛选。', '',
    '| Issue | 处理 |', '|---|---|']
for item in entries:
    if item['status'] == '排除':
        number = item['issue']
        reason = '正文复核为交互建议，移出故障范围。' if number == 259 else '候选筛选已排除：非本次故障范围。'
        out.append(f'| [#{number}](https://github.com/jimmgreen/pulse/issues/{number}) | {reason} |')

TARGET.write_text('\n'.join(out)+'\n', encoding='utf-8')
result = {'scope':data['scope'], 'head':data['review_head'], 'candidate_count':168,
          'in_scope_count':149, 'pre_repair_counts':dict(counts), 'issues':entries,
          'repairs':repairs, 'followup':followup, 'storage_optimization_review':storage_review,
          'sevenzip_menu_followup':sevenzip,
          'index_maintenance_followup':maintenance,
          'performance_investigation_followup':performance,
          'completion_followup':completion,
          'code_quality_review':quality_review,
          'quality_release':quality_release,
          'drag_viewport':drag_viewport,
          'edit_surface':edit_surface,
          'font_flicker':font_flicker,
          'text_render_implementation':text_render,
          'release_1_2_1_0':release_1210,
          'repair_scope':'Only the explicitly confirmed local subissues; baseline status is historical.'}
(HERE/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'report':str(TARGET),'counts':dict(counts),'rows':149,'excluded':19},ensure_ascii=False))
