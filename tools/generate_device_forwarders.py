"""Generate plain forwarding signatures from the locally installed Windows SDK."""
import re
import sys
from pathlib import Path

sdk = Path(sys.argv[1]).read_text()
body = sdk.split('DECLARE_INTERFACE_(IDirect3DDevice9, IUnknown)', 1)[1].split('#ifdef D3D_DEBUG_INFO', 1)[0]
custom = set('QueryInterface AddRef Release TestCooperativeLevel GetDirect3D Reset Present CreateTexture CreateVolumeTexture CreateCubeTexture CreateVertexBuffer CreateIndexBuffer UpdateTexture UpdateSurface GetRenderTargetData StretchRect ColorFill SetRenderTarget SetDepthStencilSurface GetTexture SetTexture SetSamplerState GetSamplerState CreateStateBlock BeginStateBlock EndStateBlock SetStreamSource GetStreamSource SetIndices GetIndices DrawPrimitive DrawIndexedPrimitive DrawPrimitiveUP DrawIndexedPrimitiveUP ProcessVertices'.split())
lines = ['// Generated from Windows SDK IDirect3DDevice9 signatures; no game code.']
for match in re.finditer(r'STDMETHOD(?:\((\w+)\)|_\((\w+),\s*(\w+)\))\(THIS(?:_\s*(.*?))?\)\s*PURE;', body, re.S):
    name, result, name2, args = match.groups()
    name, result, args = name or name2, result or 'HRESULT', ' '.join((args or '').split())
    params = args.split(',') if args else []
    params = [p + f' arg{i}' if re.fullmatch(r'(?:CONST )?\w+\s*[*&]*', p.strip()) else p for i, p in enumerate(params)]
    args = ','.join(params)
    names = [re.search(r'(\w+)(?:\[.*?\])?$', p.strip()).group(1) for p in params]
    declaration = f'{result} STDMETHODCALLTYPE {name}({args}) override'
    lines.append(declaration + (';' if name in custom else f' {{ return real->{name}({", ".join(names)}); }}'))
assert len(lines) == 120, len(lines)
Path(__file__).resolve().parents[1].joinpath('src/modern_device_methods.h').write_text('\n'.join(lines)+'\n')
