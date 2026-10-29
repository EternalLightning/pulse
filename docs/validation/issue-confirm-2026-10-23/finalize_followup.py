"""Merge follow-up evidence into the existing audit; no product operations."""
import hashlib
import json
import pathlib
import runpy

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def read(name):
    return json.loads((HERE / name).read_text(encoding='utf-8-sig'))


def digest(path):
    return hashlib.sha256((ROOT / path).read_bytes()).hexdigest()


menu = read('followup-menu.json')
editor = read('fixes-user-followup-ui.json')
devices = read('followup-removable.json')
audit = read('followup-index-audit.json')
assert editor['status'] == 'completed'
assert editor['after']['fail'] == 0
tests = list(menu['tests']) + [{
    'name': '重命名拖选与筛选 Enter/Esc',
    'passed': editor['after']['pass'], 'failed': editor['after']['fail'],
    'log': editor['after']['log'],
}] + devices['tests'] + [
    {'name': '网络重叠根边界与回收错误码映射', 'passed': 2, 'failed': 0,
     'log': audit['runtime_evidence']['fixture_run_log']},
    {'name': '回收拒绝后的重新确认、拒绝与后端取消', 'passed': 8, 'failed': 0,
     'log': audit['runtime_evidence']['delete_confirmation_fixture']['run_log']},
]
assert all(item['failed'] == 0 and (ROOT / item['log']).is_file() for item in tests)
display_names = {
    'admin composition and invocation': '管理员菜单组成与调用',
    'admin localized menu rendering': '管理员菜单本地化渲染',
    'existing Open with regression': '原打开方式回归',
    'asynchronous live menu regression': '异步实时菜单回归',
    'removable_index_policy_test': '可移动设备策略与临时文件名扫描',
    'removable_search_routing_test_final_production_objects': '应用搜索入口与设置渲染',
    'removable_content_cache_test_final_legacy_writer_object': '离线内置盘配置与缓存保护',
}
tests = [dict(item, name=display_names.get(item['name'], item['name'])) for item in tests]
pulse_hash = digest('build/Pulse.exe')
index_hash = digest('build/Pulse.Index.exe')
outcome = {
    'date': '2026-10-23',
    'scope': '原勾选 5 条中，用户明确 #290 为误勾选，已取消；本次选择为 #314、#308、#302、#374。另处理用户明确提出的重命名拖选闪烁、可移动设备策略、管理员菜单位置、筛选 Esc 提示音。勾选和已有针对代码均不替代本地故障确认。以下为追加核验，不改变首轮历史审计计数。',
    'changes': [
        {'name': '重命名拖选闪烁',
         'change': '修复前 16 次鼠标移动各触发一次原生重绘关闭/开启及 WS_VISIBLE 丢失。改为整次捕获拖动共用一次重绘抑制，保留原生实时选区和 LumaText 呈现；补齐释放、失捕获和失去按键状态的收尾。最终 16 次原生选区更新保留，移动期间重绘切换和可见状态丢失均为 0。',
         'evidence': 'docs/validation/issue-confirm-2026-10-23/fixes-user-followup-ui.json',
         'limits': '22 项使用实际生产处理器、原生 EDIT 和 LumaText，选区截图已检查；没有物理鼠标录像、真实 IME 或实际文件重命名验收。'},
        {'name': '可移动及 USB 外接存储；#314 / #308',
         'change': '统一持久索引设备策略，排除 DRIVE_REMOVABLE 及包括 Windows 固定磁盘在内的 USB 外接存储。旧开关、手动根和后台写入不能重新启用；设置页禁用对应选项并显示简短状态。复用现有工作线程扫描器保留已识别设备当前目录内文件名及内容临时搜索，分页、取消和 UNC 路由定向检查通过。旧离线/未知内置根保留配置和缓存，只暂停不允许的扫描。',
         'evidence': 'docs/validation/issue-confirm-2026-10-23/followup-removable.json',
         'limits': '策略注入和真实私有 NTFS 目录扫描，不等于真实 exFAT/USB 热插拔验收。MTP 原本不属于盘符文件系统搜索后端，未增加手机搜索能力。原 exFAT 持久索引现象未复现，不据此称原 issue 已修复。'},
        {'name': '以管理员身份运行',
         'change': '将已注册的直接 runas 操作提升至最外层“打开”之后、“打开方式”之前，保留偏好筛选、调用绑定、缓存与 COM 会话有效性约束。',
         'evidence': 'docs/validation/issue-confirm-2026-10-23/followup-menu.json',
         'limits': '15 项菜单组成/调用、27 项渲染、原打开方式 14 项及异步菜单 9 项通过；8 张中英文、100/150%、深浅主题截图已检查。没有真实提权或注册表修改。'},
        {'name': 'Ctrl+F 后 Esc 提示音',
         'change': 'FilterEditProc 消费已处理 Enter/Esc 后队列中的 WM_CHAR，阻止字符继续到达原生 EDIT 默认错误提示音路径。普通输入及同步逻辑保留。',
         'evidence': 'docs/validation/issue-confirm-2026-10-23/fixes-user-followup-ui.json',
         'limits': '包含于上述 22 项，未重复累计；未播放和听取实际系统音频。'},
        {'name': '#302 索引体积',
         'change': '读取原文并核查真实代码。aggregate 的目录哈希精确等于用户给出的 9ACBEFD40DDAC9E8；同一 Engine 将总索引再序列化成分卷查询副本，两类各有 A/B 快照。查询在两种表示间选择，不会因此相加返回两套结果。确认重复持久化表示，不认定重复扫描或重复活动文件记录。',
         'evidence': 'docs/validation/issue-confirm-2026-10-23/followup-index-audit.json',
         'limits': '目录及精确文件访问均遭 Windows 拒绝，未实测字节和活动代。没有修改存储策略或删除旧数据；减少分卷副本的方案仍需查询、增量、恢复和性能对照。'},
        {'name': '#374 大目录回收失败',
         'change': '生产 RunDelete 在隔离 IPC 注入 SIZE_TOO_BIG 后进入新的确切根永久删除确认。拒绝确认不发永久删除；接受才发新 token 请求；后端取消不产生虚假完成/撤销。普通访问拒绝不错误升级为永久删除。8 项通过，未发现该路径需要修复的本地同类故障。',
         'evidence': 'docs/validation/issue-confirm-2026-10-23/followup-index-audit.json',
         'limits': '未改删除产品代码；未启动真实 Shell、填满真实回收站或删除用户文件。原目录/系统 HRESULT 未取得，不称原大目录场景已验收。'},
    ],
    'tests': tests,
    'passed': sum(item['passed'] for item in tests),
    'failed': sum(item['failed'] for item in tests),
    'production_sha256': pulse_hash,
    'index_sha256': index_hash,
    'build': f'追加修改后 pulse 与 pulse_index 定向构建通过（build-final-policy.log）；状态文字缩短后 pulse 再构建通过（build-final-labels.log），无新增编译警告。最终 Pulse.exe SHA256 `{pulse_hash}`；Pulse.Index.exe SHA256 `{index_hash}`。编辑器夹具使用其证据记录的统一编辑器对象；之后仅索引边界及资源重链，没有改变这些编辑器对象。未运行完整 release/selftest，未部署或替换安装版。',
    'index_size': '现场容量均为用户观察：D:/AppData/Index 约 2.16 GB；v9/9ACBEFD40DDAC9E8 与 v9/volumes 各约 1.08 GB；前者 base-a.bin 约 565 MB；everything.db 约 100 MB。并非工具读取的精确字节数，不将不同数据集合及索引结构的容量直接作为重复记录证明。目录哈希及两套表示的源码证据解释了占用结构，未核实现场各文件的活动代或文件身份。根目录 Volumes 的活跃增量 WAL 与 v9/volumes 查询副本不同，不能按相似名称清理。',
    'remaining': '留待下次交互：#302 是否采用减少分卷副本的存储方案及所需对照验证；原大目录 Shell 现场返回值如以后可安全取得再与 #374 比较。本轮不再向用户提问，不修改目录 ACL、所有者、真实配置或服务，不删除现有索引。#290 已按用户更正排除本次处理。',
    'issue_treatment': {
        '314': '按用户要求排除可移动/USB 持久索引，保留临时搜索；未复现原 exFAT 持久索引故障',
        '308': '同 #314；设备策略及临时搜索定向通过，原场景未作硬件验收',
        '302': '确认 aggregate+pervolume 两套持久表示；活动重复扫描/记录未确认，未改存储策略',
        '374': '8 项隔离确认/取消检查通过；未发现需修同类故障，原大目录未复测',
        '290': '用户明确误勾选，本次取消选择、未修复',
    },
}
(HERE / 'followup-outcomes.json').write_text(json.dumps(outcome, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
menu['production_build'] = {'log': 'bench_data/issue-followup-main/build-final-labels.log', 'pulse_sha256': pulse_hash}
(HERE / 'followup-menu.json').write_text(json.dumps(menu, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
runpy.run_path(str(HERE / 'compile_report.py'), run_name='__main__')
print(json.dumps({'followup_passed': outcome['passed'], 'followup_failed': outcome['failed']}, ensure_ascii=False))
