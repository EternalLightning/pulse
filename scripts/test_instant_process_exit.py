# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

import os, subprocess, time, ctypes, json, zipfile, sys
from pathlib import Path
root=Path(__file__).resolve().parent.parent
out=root/'build_realtime'
mode='complete' if '--complete' in sys.argv else ('hung-suspend' if '--hung-suspend' in sys.argv else ('suspend' if '--suspend' in sys.argv else 'crash'))
ready=out/('instant-'+mode+'-ready.txt')
for marker in [Path(str(ready)+'.stop'),Path(str(ready)+'.stopped')]:
    if marker.exists(): marker.unlink()
if ready.exists(): ready.unlink()
env=os.environ.copy();env['PULSE_TEST_INSTANT_MODE']='1';env['PULSE_TEST_INSTANT_CRASH_READY']=str(ready);env.pop('PULSE_TEST_INSTANT_IDLE_SECONDS',None)
log=open(out/('instant-'+mode+'-child.log'),'w')
p=subprocess.Popen([str(out/'pulse_content_index_test.exe')],env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=0x08000000)
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[ctypes.c_uint32,ctypes.c_int,ctypes.c_uint32];k.OpenProcess.restype=ctypes.c_void_p
k.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];k.CloseHandle.argtypes=[ctypes.c_void_p]
handles=[]
try:
    until=time.monotonic()+30
    while not ready.exists() and p.poll() is None and time.monotonic()<until: time.sleep(.025)
    if not ready.exists(): raise RuntimeError('crash child did not reach live subscription')
    ids=[int(x) for x in ready.read_text().splitlines()]
    for pid in ids:
        h=k.OpenProcess(0x100000,False,pid)
        if h: handles.append(h)
    fixture=next((root/'bench_data').glob('content-index-'+str(p.pid)+'-*'))
    doc=fixture/'files'/'long.docx'
    with zipfile.ZipFile(doc,'w',compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr('[Content_Types].xml','<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
        z.writestr('_rels/.rels','<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
        z.writestr('word/document.xml','<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>'+('<w:p><w:r><w:t>ordinary text without match</w:t></w:r></w:p>'*250000)+'</w:body></w:document>')
    # Read only this test parent's descendants from the system process snapshot.
    class ENTRY(ctypes.Structure):
        _fields_=[('size',ctypes.c_ulong),('usage',ctypes.c_ulong),('pid',ctypes.c_ulong),('heap',ctypes.c_void_p),('module',ctypes.c_ulong),('threads',ctypes.c_ulong),('parent',ctypes.c_ulong),('priority',ctypes.c_long),('flags',ctypes.c_ulong),('exe',ctypes.c_wchar*260)]
    k.CreateToolhelp32Snapshot.argtypes=[ctypes.c_ulong,ctypes.c_ulong];k.CreateToolhelp32Snapshot.restype=ctypes.c_void_p
    k.Process32FirstW.argtypes=[ctypes.c_void_p,ctypes.POINTER(ENTRY)];k.Process32NextW.argtypes=k.Process32FirstW.argtypes
    doc_pid=None;until=time.monotonic()+10
    while not doc_pid and time.monotonic()<until:
        snap=k.CreateToolhelp32Snapshot(2,0);e=ENTRY();e.size=ctypes.sizeof(e)
        ok=k.Process32FirstW(snap,ctypes.byref(e))
        while ok:
            if e.parent in ids and e.exe.lower()=='pulse.document.exe': doc_pid=e.pid;break
            ok=k.Process32NextW(snap,ctypes.byref(e))
        k.CloseHandle(snap)
        if not doc_pid: time.sleep(.01)
    if doc_pid:
        h=k.OpenProcess(0x100000,False,doc_pid)
        if h: handles.append(h)
    if mode=='complete':
        if not doc_pid or len(handles)!=len(ids)+1: raise RuntimeError('document handle unavailable')
        completed=k.WaitForSingleObject(handles[-1],15000)==0
        alive=all(k.WaitForSingleObject(h,0)==258 for h in handles[:-1])
        result={'document_pid':doc_pid,'document_exited_after_completion':completed,'light_subscription_remains':alive}
        print(json.dumps(result));(out/'instant-complete-result.json').write_text(json.dumps(result,indent=2))
        Path(str(ready)+'.stop').write_text('stop');p.wait(timeout=3)
        if not completed or not alive: raise RuntimeError('completion resource release acceptance failed')
        sys.exit(0)
    if mode=='hung-suspend':
        nt=ctypes.WinDLL('ntdll');nt.NtSuspendProcess.argtypes=[ctypes.c_void_p]
        for pid in ids:
            h=k.OpenProcess(0x0800,False,pid)
            if not h or nt.NtSuspendProcess(h)!=0: raise RuntimeError('failed to suspend owned fixture agent')
            k.CloseHandle(h)
    start=time.monotonic()
    if mode!='crash':
        Path(str(ready)+'.stop').write_text('stop')
    else:
        p.kill();p.wait(timeout=2)
    exited=[k.WaitForSingleObject(h,max(0,int(2000-(time.monotonic()-start)*1000)))==0 for h in handles]
    result={'parent_pid':p.pid,'agent_pids':ids,'document_pid':doc_pid,'exited':exited,'all_exited':all(exited),'elapsed_ms':round((time.monotonic()-start)*1000,2)}
    print(json.dumps(result));(out/('instant-'+mode+'-result.json')).write_text(json.dumps(result,indent=2))
    if not doc_pid or not handles or not all(exited) or result['elapsed_ms']>=2000: raise RuntimeError('crash cleanup acceptance failed')
    if mode!='crash': p.wait(timeout=3)
finally:
    if p.poll() is None:p.kill();p.wait()
    for h in handles:k.CloseHandle(h)
    log.close()
