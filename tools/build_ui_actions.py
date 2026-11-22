"""Compile only our own AS2 extension; no original game asset is a build input."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
from avm1_literals import inline_constants

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--java', required=True)
p.add_argument('--ffdec', required=True)
p.add_argument('--work', type=Path, required=True)
p.add_argument('--source', default='ui_layout.as')
p.add_argument('--symbol', default='uiLayoutActions')
p.add_argument('--header', default='ui_layout_bytes.h')
p.add_argument('--marker', default='__PrototypeModernLayout')
a = p.parse_args()
a.work.mkdir(parents=True, exist_ok=True)
scripts = a.work / 'scripts/frame_1'
scripts.mkdir(parents=True, exist_ok=True)
source = (root / 'src' / a.source).read_bytes()
(scripts / 'DoAction.as').write_bytes(source)
bits = '10000' + ''.join(f'{v:016b}' for v in [0,25600,0,14400])
bits += '0' * (-len(bits) % 8)
rect = int(bits, 2).to_bytes(len(bits)//8, 'big')
body = rect + struct.pack('<HH', 60*256, 1) + struct.pack('<H', 12*64+1) + b'\0' + struct.pack('<HH', 64, 0)
blank = a.work / 'own-blank.swf'
blank.write_bytes(b'FWS\x08' + struct.pack('<I',len(body)+8) + body)
compiled = a.work / 'own-layout.swf'
result = subprocess.run([a.java, '-Djava.awt.headless=true', '-jar', a.ffdec,
    '-importScript', str(blank), str(compiled), str(a.work / 'scripts')], capture_output=True)
(a.work / 'compile.log').write_bytes(result.stdout + result.stderr)
result.check_returncode()
data = compiled.read_bytes()
assert data[:3] == b'FWS'
at = 8 + len(rect) + 4
actions = []
tags = []
while at < len(data):
    h, = struct.unpack_from('<H', data, at); at += 2
    tag, size = h >> 6, h & 63
    if size == 63: size, = struct.unpack_from('<I',data,at); at += 4
    if tag == 12: actions.append(data[at:at+size])
    tags.append((tag, data[at:at+size]))
    at += size
assert len(actions) == 1 and (a.marker+'\0').encode() in actions[0] and actions[0][-1] == 0
code = inline_constants(actions[0])
(a.work / 'ui-layout.avm1').write_bytes(code)
literal_body = data[8:8+len(rect)+4]
for tag, payload in tags:
    if tag == 12: payload = code
    size = len(payload)
    literal_body += struct.pack('<H', tag*64 + min(size, 63))
    if size >= 63: literal_body += struct.pack('<I', size)
    literal_body += payload
(a.work / 'own-literals.swf').write_bytes(b'FWS\x08' + struct.pack('<I', len(literal_body)+8) + literal_body)
header = f'// Generated from our src/{a.source} by tools/build_ui_actions.py.\n#pragma once\n'
header += '// AS2 SHA256: ' + hashlib.sha256(source).hexdigest() + '\n'
header += f'static const unsigned char {a.symbol}[]={{\n'
header += '\n'.join(','.join(f'0x{b:02x}' for b in code[i:i+20])+',' for i in range(0,len(code),20))
header += '\n};\n'
(root / 'src' / a.header).write_text(header,encoding='utf-8')
print(f'Compiled own UI extension: {len(code)} bytes, no constant-pool mutation; no original asset read or modified.')
