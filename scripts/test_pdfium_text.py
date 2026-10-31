# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""Small generated PDFs exercise searchable text, page boundaries and error states."""
import json, os, subprocess
from pathlib import Path
root=Path(__file__).resolve().parent.parent
base=root/'bench_data/pdfium-text-fixtures'
base.mkdir(parents=True,exist_ok=True)
os.environ['PULSE_DOCUMENT_ADMISSION_DIR']=str(base)
def pdf(name,streams,font=None,extra=None,encrypted=False):
    objects=[b'<< /Type /Catalog /Pages 2 0 R >>',b'',font or b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    kids=[]
    for content in streams:
        page=len(objects)+1;kids.append(f'{page} 0 R')
        objects.append(f'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 3 0 R >> >> /Contents {page+1} 0 R >>'.encode())
        objects.append(b'<< /Length '+str(len(content)).encode()+b' >>\nstream\n'+content+b'\nendstream')
    objects[1]=f'<< /Type /Pages /Count {len(kids)} /Kids [{" ".join(kids)}] >>'.encode()
    if extra: objects.extend(extra)
    encrypt_ref=''
    if encrypted:
        objects.append(b'<< /Filter /Standard /V 1 /R 2 /Length 40 /O <'+b'00'*32+b'> /U <'+b'00'*32+b'> /P -4 >>')
        encrypt_ref=f'/Encrypt {len(objects)} 0 R /ID [<'+('00'*16)+'> <'+('00'*16)+'>]'
    output=b'%PDF-1.7\n';offsets=[0]
    for i,obj in enumerate(objects,1):offsets.append(len(output));output+=f'{i} 0 obj\n'.encode()+obj+b'\nendobj\n'
    start=len(output);output+=f'xref\n0 {len(objects)+1}\n0000000000 65535 f \n'.encode()
    for offset in offsets[1:]:output+=f'{offset:010} 00000 n \n'.encode()
    output+=f'trailer\n<< /Root 1 0 R /Size {len(objects)+1} {encrypt_ref} >>\nstartxref\n{start}\n%%EOF\n'.encode()
    p=base/name;p.write_bytes(output);return p
def run(path,needle=''):
    bench=os.environ.get('PULSE_PDF_BENCH',str(root/'build_realtime/pulse_pdf_bench.exe'))
    proc=subprocess.run([bench,'pdfium',str(path),needle],capture_output=True,timeout=35)
    head,body=proc.stdout.split(b'\n',1);return json.loads(head),body.decode('utf-8')
checks=[]
def check(ok,name):
    checks.append(ok);print('[PASS]' if ok else '[FAIL]',name,flush=True)
p=pdf('多页.pdf',[b'BT /F1 20 Tf 20 700 Td (alpha) Tj ET',b'BT /F1 20 Tf 20 700 Td (beta TailNeedle) Tj ET'])
m,t=run(p);check(m['ok'] and 'alpha' in t and 'TailNeedle' in t,'all pages including tail searchable')
check('alphabeta' not in t,'page boundary cannot fabricate concatenated word')
m,t=run(p,'tailneedle');check(m['ok'] and 'TailNeedle' in t,'case insensitive stop needle reaches final page')
p=pdf('空白.pdf',[b'']);m,t=run(p);check(not m['ok'] and m['error']==50,'no text is unsupported, not a fully searched nonmatch')
bad=base/'损坏.pdf';bad.write_bytes(b'%PDF-1.7 invalid');m,t=run(bad);check(not m['ok'] and m['error']==11,'malformed PDF reports format error')
p=pdf('密码保护.pdf',[b''],encrypted=True);m,t=run(p);check(not m['ok'] and m['error']==5,'password-required PDF header reports access denied')
cmap=b'/CIDInit /ProcSet findresource begin 12 dict begin begincmap /CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def /CMapName /Test def /CMapType 2 def 1 begincodespacerange <0000> <FFFF> endcodespacerange 3 beginbfchar <0001> <4E2D> <0002> <6587> <0003> <D840DC00> endbfchar endcmap CMapName currentdict /CMap defineresource pop end end'
font=b'<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light /Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 7 0 R >>'
p=pdf('中文扩展区.pdf',[b'BT /F1 20 Tf 20 700 Td <000100020003> Tj ET'],font,[b'<< /Type /Font /Subtype /CIDFontType0 /BaseFont /STSong-Light /CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 4 >> /DW 1000 >>',b'<< /Length '+str(len(cmap)).encode()+b' >>\nstream\n'+cmap+b'\nendstream'])
m,t=run(p);check(m['ok'] and '中文' in t,'Chinese ToUnicode text survives extraction');check(chr(0x20000) in t,'supplementary Chinese character survives extraction')
# Use real CJK glyphs for the line-layout case. Identity-H CID 1 above has a
# space-like glyph in STSong: ToUnicode changes its text, not its geometry, so
# PDFium can discard that glyph when it occupies a line by itself.
line_font=b'<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light /Encoding /UniGB-UCS2-H /DescendantFonts [6 0 R] >>'
# Indent the second line so PDFium emits a CRLF instead of interpreting the
# aligned single-glyph lines as one vertical CJK run. The phrase crosses it.
p=pdf('中文断行.pdf',[b'BT /F1 20 Tf 20 700 Td <4E2D> Tj 20 -40 Td <6587> Tj ET'],line_font,[b'<< /Type /Font /Subtype /CIDFontType0 /BaseFont /STSong-Light /CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 4 >> /DW 1000 /FontDescriptor 7 0 R >>',b'<< /Type /FontDescriptor /FontName /STSong-Light /Flags 6 /FontBBox [-25 -254 1000 880] /ItalicAngle 0 /Ascent 880 /Descent -254 /CapHeight 700 /StemV 80 >>'])
m,t=run(p);check(m['ok'] and '中文' in t,'CJK glyph line breaks within a page remain searchable as a phrase')
(root/'build_realtime/pdfium-text-test.json').write_text(json.dumps({'passed':all(checks),'checks':len(checks)}))
raise SystemExit(0 if all(checks) else 1)
