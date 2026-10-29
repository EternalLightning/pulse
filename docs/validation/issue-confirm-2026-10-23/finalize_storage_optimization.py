"""Merge measured storage/device results into the single existing audit report."""
import hashlib
import json
import pathlib
import re
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LOG = ROOT / 'bench_data/index-storage-optimization/paired-run.log'
raw = LOG.read_text(encoding='utf-8')
assert '[FAIL]' not in raw
metrics = []
for line in raw.splitlines():
    if line.startswith('[METRIC]'):
        row = {}
        for key, value in re.findall(r'(\w+)=([^ ]+)', line):
            try:
                row[key] = float(value) if '.' in value else int(value)
            except ValueError:
                row[key] = value
        metrics.append(row)
snapshots = [r for r in metrics if r.get('generations') == 2]
assert len(snapshots) == 2
old, new = snapshots
reduction = 100 * (1 - new['snapshot_logical_bytes'] / old['snapshot_logical_bytes'])
passed = sum(line.startswith('[PASS]') for line in raw.splitlines())
paired = re.findall(r'\[PASS\] ([\w-]+) paired .* cases=(\d+)', raw)
assert paired == [('baseline', '97'), ('overlay', '97'), ('WAL-restart', '97')]
device = json.loads((HERE / 'exfat-device-validation.json').read_text(encoding='utf-8'))
assert device['passed'] == 29 and device['failed'] == 0 and device['fixture_cleaned']
def sha(path):
    return hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
def write(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
def link(path, label):
    return f'[{label}]({ROOT.as_posix()}/{path})'
evidence = {
    'date': '2026-10-23',
    'scope': 'Disable derived filename query copies for production; retain canonical snapshot, A/B and real WAL. No installation, service/config/ACL changes or existing production index cleanup.',
    'implementation': {'default_use_volume_query_shards': False,
        'legacy_access': 'private EngineTestAccess friend only; no environment, CLI or user preference override',
        'guards': ['WriteVolumeShards before any directory creation/write', 'RefreshQueryShardsLocked clears mappings then returns before directory/manifest resolution']},
    'build': {'targets': ['pulse_index', 'pulse', 'pulse_index_storage_test'],
        'exit_code': 0, 'new_compiler_warnings': 0,
        'logs': ['bench_data/index-storage-optimization/build-production.log', 'bench_data/index-storage-optimization/configure.log', 'bench_data/index-storage-optimization/build-fixture-final.log']},
    'test': {'exit_code': 0, 'passed_top_level_checks': passed, 'failed': 0,
        'paired_query_cases': {phase: int(count) for phase, count in paired},
        'files': 60000, 'volumes': 2, 'directories': 48, 'generations': 2,
        'log': str(LOG.relative_to(ROOT)).replace('\\', '/'),
        'fixture': raw.splitlines()[0].split(': ', 1)[1], 'retained': True},
    'metrics': metrics, 'two_generation_snapshot_reduction_percent': reduction,
    'exfat_evidence': 'docs/validation/issue-confirm-2026-10-23/exfat-device-validation.json',
    'source_and_binary_sha256': {p: sha(p) for p in ['src/index/index_engine.cpp', 'src/index/index_engine.h',
        'src/bench/index_storage_pair_test.cpp', 'src/bench/removable_exfat_device_test.cpp',
        'CMakeLists.txt', 'build/Pulse.Index.exe', 'build/Pulse.exe', 'build/pulse_index_storage_test.exe']},
    'limits': [
        'Logical snapshot bases/manifests only, excludes real WAL, filesystem allocation and temporary peak.',
        'Mapping bytes are mapped file extents; resident working set/private bytes not measured.',
        'Synthetic MFT/USN data with production engine query/persistence, no live-volume journal or full scan.',
        'Authorization callback tested; actual Windows user-token/ACL combinations not tested.',
        '30 warm mixed-query samples per mode/phase, legacy measured first; no cold-cache or broad latency guarantee.',
        'Active-base corruption fallback tested, not every interruption/power-loss/locked-file condition.',
        'No new pinyin-specific, offline/online hotplug, feed, old-format or single-volume rebuild regression run.',
        'Existing v9/volumes remains unchanged; protected production directory cannot be inventoried.',
        'Installed program/service not replaced; no manual UI acceptance or physical USB unplug.']}
write(HERE / 'storage-optimization-implementation.json', evidence)

review_path = HERE / 'storage-optimization-review.json'
review = json.loads(review_path.read_text(encoding='utf-8'))
review.setdefault('pre_implementation_source_fingerprints', review['source_fingerprints'].copy())
review['source_fingerprints'] = {p: sha(p) for p in review['source_fingerprints']}
review['scope'] = evidence['scope'] + ' Paired synthetic engine and real exFAT backend validation completed.'
review['evidence_levels']['not_verified'] = '; '.join(evidence['limits'])
review['evidence_levels']['future_validation'] = 'Controlled installed migration/cleanup and real UI acceptance; evaluate further compression only after component profiling.'
review['implementation_evidence'] = 'docs/validation/issue-confirm-2026-10-23/storage-optimization-implementation.json'
lines = review['markdown_lines']
lines[0] = '日期：2026-10-23。本节按用户要求展开 #302，现已完成第一阶段源码优化、构建、双路径对照及真实 exFAT U 盘后端验证。仍使用同一份报告。安装版与真实索引目录未替换或清理。'
for i, text in enumerate(lines):
    if text.startswith('**已经确认：'):
        lines[i] = '**实现前已经确认：同一个机器索引 Engine 保存 aggregate 总索引，再生成完整分卷查询副本，两类各保留 A/B 两代。现在生产默认停止生成和加载分卷查询副本，直接使用既有 aggregate 查询路径。** 这解决重复持久表示；不能据此认定存在重复扫描或重复活动结果。'
    elif text.startswith('**建议先评估'):
        lines[i] = f'**第一阶段已实现并通过定向验证：保留 aggregate、A/B 和真实 WAL。** 6 万文件双卷双代隔离快照逻辑大小由 64,517,120 降至 31,731,060 字节，减少 {reduction:.1f}%。现有旧副本保留且不再读写，所以用户当前 2.16G 目录尚未直接缩小。后续先做可审查的迁移回收，再评估单份快照压缩。'
    elif text.startswith('`AggregateShardPaths` 使用'):
        lines[i] = text.replace('`WriteVolumeShards` 从总索引', '实现前的 `WriteVolumeShards` 从总索引').replace('构建、合并和单卷恢复后都存在这条发布路径。', '构建、合并和单卷恢复仍调用此入口；新默认策略在创建目录或写入前直接返回。保留旧实现仅供私有 friend 夹具比较，没有用户设置或环境变量启用入口。')
    elif text.startswith('`RefreshQueryShardsLocked` 要求'):
        lines[i] = '新默认策略先清空已有分卷映射和 ready 标志，再于任何路径创建、manifest 解析前返回。旧实现仍由私有夹具启用，要求总索引已存在且各卷匹配，失败时回退 aggregate。`QueryNode/Attr/Name` 与倒排选择沿用原有 aggregate 路径。主索引节点、FRN/USN、授权过滤和协议保持原实现。' + link('src/index/index_engine.cpp:1936', '映射入口') + '。'
    elif text.startswith('**阶段一：'):
        lines[i] = '**阶段一已完成：减少整套派生副本，保留主格式与恢复。** 统一私有策略默认 false，同时控制 WriteVolumeShards 和 RefreshQueryShardsLocked。只在测试 friend 内启用旧路径作对照；不增加用户选项。未更改快照版本、全局 ID、A/B、真实 WAL、USN 或 feed 协议。运行结果见下节。'
    elif text.startswith('| aggregate 为主，停用派生分卷快照'):
        lines[i] = '| aggregate 为主，停用派生分卷快照 | 利用既有查询路径，保留格式及恢复 | 本轮已实施；定向结果见下节 |'
    elif text.startswith('本轮未授权或执行'):
        lines[i] = text
    elif '本轮未授权或执行任何现存索引删除' in text:
        lines[i] = text.replace('本轮未授权或执行任何现存索引删除', '本轮没有执行任何现存索引删除')
start = lines.index('### 8. 需要怎样的验证才能实施') if '### 8. 需要怎样的验证才能实施' in lines else next(i for i, t in enumerate(lines) if t.startswith('### 8.'))
lines = lines[:start] + [
    '### 8. 本轮实际验证与测量', '',
    f'专用 `pulse_index_storage_test`：**{passed} 项顶层检查 PASS / 0 FAIL，退出码 0**，其中包含 baseline、overlay、WAL-restart 三组各 97 个查询比较。数据为两个合成 MFT 卷、6 万文件、48 个目录，未扫描真实内置磁盘。旧模式明确断言两个分卷映射生效，新模式断言无分卷映射。' + link('bench_data/index-storage-optimization/paired-run.log', '原始日志') + '。', '',
    '| 检查 | 实际结果 |', '|---|---|',
    '| 命中与分页 | 独立算术核对总数及 alpha/report/TXT 数量；比较完整列表、名称/大小/时间/索引顺序正反序、页切片、过滤、范围、排名、授权回调与取消 |',
    '| 更新与重启 | 合成 USN 触发真实目录移动、文件重命名/删除/新增；实际逐卷 WAL 刷盘后通过 TryLoadCache 重启回放，节点路径和各卷 USN 游标通过 |',
    '| 旧副本与新写入 | 全新输出从未生成 v9/volumes；带有有效/陈旧分卷 manifest 的旧输出可加载，新模式分卷映射为 0，发布后旧副本所有文件长度及 CRC 均不变 |',
    '| 恢复 | 两模式都保留总索引两代；破坏活动快照后回退上一代，拒绝不匹配的新代 WAL，并进入已有恢复状态 |', '',
    '| 相同数据的测量口径 | 旧模式 | 新模式 |', '|---|---:|---:|',
    '| 一代快照逻辑字节（含 manifest） | 32,259,224 | 15,865,748 |',
    '| 两代快照逻辑字节（含 manifest） | 64,517,120 | 31,731,060 |',
    '| 活动总索引映射长度 | 15,865,312 | 15,865,312 |',
    '| 活动分卷查询副本映射长度 | 16,389,792 | 0 |',
    '| 两次合成建树及发布耗时 | 630.797 / 639.363 ms | 328.597 / 372.523 ms |',
    '| 基线查询 median / P95 | 12.872 / 33.242 ms | 13.799 / 35.244 ms |',
    '| 覆盖层查询 median / P95 | 21.027 / 42.084 ms | 19.473 / 40.741 ms |', '',
    '空间为快照文件逻辑长度，未计真实 WAL、分配簇和临时峰值。映射长度不等于 Working Set 或 Private Bytes，没有据此宣称 RAM 减半。查询各模式/阶段先预热 3 次，再计时 30 次，轮换 alpha、*.txt、record 的名称排序首屏/分页查询，并清理 Engine 查询缓存；OS 文件缓存没有清空，旧模式先测。这一组 P95 分别约 +6.0% 和 −3.2%，没有观察到超过拟议 10% 的回退，但不是全面性能保证，也不能代替大规模安装现场基线。两次发布耗时不是整盘扫描耗时。', '',
    '`pulse_index`、`pulse` 和专用测试目标均构建成功，无新编译警告。主程序 SHA256 保持此前值；索引程序 SHA256 为 `' + evidence['source_and_binary_sha256']['build/Pulse.Index.exe'] + '`。没有运行完整 release/selftest；没有将历史测试标成这次重新通过。' + link('docs/validation/issue-confirm-2026-10-23/storage-optimization-implementation.json', '结构化结果与哈希') + '。', '',
    '### 9. exFAT U 盘实际结果', '',
    '**真实 G:\\ exFAT U 盘，29 PASS / 0 FAIL。** Win32 识别为 DRIVE_REMOVABLE，生产探测在线且 removable_bus=true，STORAGE 描述符 BusType=7（USB）；不是根据盘符猜测。', '',
    '| 范围 | 实际结果 |', '|---|---|',
    '| 持久排除 | 实际设备与子目录均拒绝持久索引；共享内容根排除该卷；隔离配置保存被拒且数据库未创建、原 sidecar 不变；旧 USB 配置只在内存过滤 |',
    '| 文件名临时搜索 | 自建唯一目录内 10 个 TXT 全部计入，分页、中文文件名、子目录、加载/完成和限定范围通过 |',
    '| 内容临时搜索 | 生产 RunContentSearch 找到 3 项实际内容匹配，含中文名称、嵌套文件和行号，无持久订阅 |',
    '| 取消与陈旧结果 | 回调边界取消、失效设备缓存、旧 generation 拦截与当前 generation 接受均通过 |',
    '| 清理 | 只创建 11 个小型数据文件及归属标记；核对唯一目录、父路径、标记与卷 GUID 后清理，并确认该目录不存在 |', '',
    link('docs/validation/issue-confirm-2026-10-23/exfat-device-validation.json', '设备与逐项证据') + '；' + link('src/bench/removable_exfat_device_test.cpp', '实盘夹具') + '。毫秒计时舍入为 0，不用作性能依据。没有整盘扫描、真实拔插或安装版 UI 手工验收；无法监控受保护的 D:/AppData/Index，因此这里只证明本轮后端/策略及隔离写入口行为，不声称全局真实索引目录零写入。', '',
    '### 10. 当前尚未完成的部分', '',
    '安装版尚未替换，现有 2.16G 目录保持原样。下一步是取得只读清单、确认 aggregate 两代与真实 WAL、在受控更新窗口释放旧映射，再按清单回收已确认的 v9/volumes 派生副本；不能直接删整个 Index，也不能把同盘改名备份计为已回收空间。当前目录权限仍限制现场清单，本轮未修改 ACL、所有者、服务或用户配置。', '',
    '本轮没有覆盖真实 Windows token/ACL 组合、完整断电/锁文件矩阵、拼音专项、feed 专项、旧格式导入、单卷重建及真实设备离线/重连。这些边界没有记作通过；影响部署或清理的检查应在迁移前补齐。单份快照的名称池/倒排压缩属于下一阶段，不能承诺缩至 Everything 的 100MB。', '']
review['markdown_lines'] = lines
write(review_path, review)

follow_path = HERE / 'followup-outcomes.json'
follow = json.loads(follow_path.read_text(encoding='utf-8'))
for item in follow['changes']:
    if item['name'] == '#302 索引体积':
        item.update(change=f'第一阶段源码已完成：同时停写、停读可由 aggregate 重建的分卷查询副本，保留主格式、A/B 和真实 WAL。6 万文件双卷双代隔离快照逻辑大小减少 {reduction:.1f}%；三组各 97 个查询对照及更新/恢复通过。', evidence=review['implementation_evidence'], limits='安装版未替换，真实 2.16G 目录未清理。逻辑大小和映射长度不是实际分配空间/RAM；现场迁移及更广泛性能验收仍待完成。')
    elif item['name'] == '可移动及 USB 外接存储；#314 / #308':
        item['limits'] = '追加真实 G:/ exFAT USB 后端检查 29 PASS / 0 FAIL，设备排除、临时搜索、取消、陈旧结果及隔离配置写入通过；证据见专项第9节。未实际拔盘或手工验收安装版 UI；MTP 手机不属于本次盘符后端。'
follow.setdefault('pre_storage_build', follow['build'])
follow['build'] = '此前追加验证的构建记录（不重标为本次重跑）：' + follow['pre_storage_build'] + ' 本次存储构建和最终索引程序哈希见下一节专项结果。'
follow['remaining'] = '尚待受控安装更新、真实索引清单与派生副本回收，以及报告明确列出的现场验收。#374 原大目录 Shell 现场返回值如以后可安全取得再比较；#290 按用户更正排除。本轮不再提问，不更改 ACL、所有者、真实配置或服务，不删除现存索引。'
follow['issue_treatment']['302'] = '已实现停写停读派生分卷副本；双路径及恢复通过；真实目录空间回收/安装验收待迁移'
follow['issue_treatment']['314'] = '按用户要求排除可移动/USB 持久索引；真实 exFAT 临时搜索与排除策略通过，原持久索引故障未复现'
follow['issue_treatment']['308'] = '同 #314；真实 exFAT 临时搜索与排除策略通过，原持久索引故障未复现'
write(follow_path, follow)
subprocess.run(['python', str(HERE / 'compile_report.py')], check=True, cwd=ROOT)
report = (ROOT / 'docs/issue-bug-audit-2026-10-23.md').read_text(encoding='utf-8')
local_links = re.findall(r'\]\((E:/Projects/pulse/[^)]+)\)', report)
for target in local_links:
    match = re.fullmatch(r'(.+):(\d+)', target)
    path = pathlib.Path(match[1] if match else target)
    assert path.exists(), target
    if match:
        assert 1 <= int(match[2]) <= len(path.read_text(encoding='utf-8').splitlines()), target
checklist = (ROOT / 'docs/issue-pending-fix-checklist-2026-10-23.md').read_text(encoding='utf-8')
assert len(re.findall(r'^- \[[ xX]\]', checklist, re.M)) == 106
selected = sorted(int(n) for n in re.findall(r'^- \[[xX]\] \[#(\d+)', checklist, re.M))
assert selected == [302, 308, 314, 374]
assert all(sha(p) == value for p, value in review['source_fingerprints'].items())
prior_validation = HERE / 'storage-optimization-review-validation.json'
prior = json.loads(prior_validation.read_text(encoding='utf-8'))
prior['stage'] = 'Historical pre-implementation documentation-only review; current implementation results are in storage-optimization-implementation-validation.json.'
write(prior_validation, prior)
write(HERE / 'storage-optimization-implementation-validation.json', {
    'document_check': 'PASS', 'local_links_valid': len(local_links),
    'current_source_hashes_verified': len(review['source_fingerprints']),
    'baseline_in_scope': 149, 'baseline_excluded': 19, 'checkboxes_preserved': 106, 'selected': selected,
    'storage_top_level_checks_passed': passed, 'storage_paired_query_comparisons': 291,
    'exfat_checks_passed': 29, 'failed': 0,
    'production_install_changed': False, 'production_index_cleaned': False})
print(json.dumps({'storage_passed': passed, 'query_comparisons': 291, 'usb_passed': 29, 'snapshot_reduction_percent': reduction}, ensure_ascii=False))
