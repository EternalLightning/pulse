# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""Paired read-only benchmarks. Extracted text stays in memory, never in reports."""
import collections, hashlib, json, os, re, subprocess, sys, time
from pathlib import Path

root=Path(__file__).resolve().parent.parent
build=root/'build_realtime'
admission=root/'bench_data/pdfium-admission';admission.mkdir(parents=True,exist_ok=True)
os.environ['PULSE_DOCUMENT_ADMISSION_DIR']=str(admission)
candidate_engine=sys.argv[3] if len(sys.argv)>3 else 'pdfium'
manifest=Path(sys.argv[1])
output=Path(sys.argv[2])
paths=json.loads(manifest.read_text(encoding='utf-8-sig'))
rows=json.loads(output.read_text(encoding='utf-8')) if output.exists() else []
for i,path in enumerate(paths):
    if any(row['path']==path for row in rows): continue
    if not Path(path).is_file(): continue
    item={'path':path,'size':Path(path).stat().st_size,'candidate_engine':candidate_engine,'runs':{}}
    texts={}
    for engine in (['ifilter','pdfium'] if i%2==0 else ['pdfium','ifilter']):
        process=subprocess.run([str(build/'pulse_pdf_bench.exe'),candidate_engine if engine=='pdfium' else engine,path,''],capture_output=True,timeout=40)
        if process.returncode: raise RuntimeError((engine,process.returncode,process.stderr[:200]))
        header,body=process.stdout.split(b'\n',1)
        values=json.loads(header)
        texts[engine]=body.decode('utf-8')
        values['body_sha256']=hashlib.sha256(body).hexdigest()
        values['needle_3d3s']='3d3s' in texts[engine].casefold()
        item['runs'][engine]=values
    # Compare actual searchable tokens from the incumbent text without publishing body text.
    baseline=texts['ifilter'].casefold(); candidate=texts['pdfium'].casefold()
    all_tokens=set(re.findall(r'[a-z0-9]{3,}|[\u4e00-\u9fff]{2,6}',baseline))
    tokens=sorted((t for t in all_tokens if len(t)<=32),key=lambda t:hashlib.sha256(t.encode()).digest())[:512]
    item['sampled_tokens']=True
    item['baseline_tokens']=len(tokens)
    item['candidate_missing_tokens']=sum(token not in candidate for token in tokens)
    normalized=re.sub(r'\s+','',candidate)
    item['candidate_missing_after_whitespace_normalization']=sum(token not in normalized for token in tokens)
    queries=['3d3s','结构','钢结构','设计','基础','混凝土','建筑','计算','yjk','model','steel','column','beam','load','concrete','software','user','manual','connection','reinforcement']
    item['query_hits']={engine:[q for q in queries if q in value.casefold()] for engine,value in texts.items()}
    item['lost_queries']=sorted(set(item['query_hits']['ifilter'])-set(item['query_hits']['pdfium']))
    rows.append(item)
    output.write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
    print(i+1,Path(path).name,'ifilter_ms',round(item['runs']['ifilter']['us']/1000,1),'pdfium_ms',round(item['runs']['pdfium']['us']/1000,1),'errors',[x['error'] for x in item['runs'].values()],'missing',item['candidate_missing_tokens'],flush=True)
