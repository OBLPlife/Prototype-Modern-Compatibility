"""Create local fingerprints from the user's original engine; never executes it."""
import hashlib
import json
from pathlib import Path
import sys
from inspect_pe import Image

root = Path(__file__).resolve().parents[1]
image = Image(sys.argv[1])
spans = [(0x2e0240, 11), (0x2e0260, 11), (0x2e0280, 5), (0x2e1050, 8),
         (0x616ee0, 8), (0x2e3cf6, 25), (0x4dfc50, 40),
         (0x2e3d12, 4), (0x2e3d19, 4), (0x2e3d20, 4), (0x2e02b1, 2), (0x617560, 8),
         (0x618288,5),(0x61837f,5),(0x2e10ec,2),(0x4dfba4,6),(0x2e0200,10),
         (0x9d763c,5),(0x618413,7),(0x2e02db,5),(0x980be0,111),
         (0x616bc7,13),(0x885e76,5),(0x4abf4c,5),
         (0x622986,5),(0x622992,5),(0x6162f6,14),(0xe74404,4),
         (0x6450b5,6),(0xab7fd9,6),(0xe70a04,4),(0xe70a0c,4),(0x6be838,5),(0x642e2b,5),
         (0x70ed74,5),(0x883993,5)]
relocations = {e.rva for block in image.pe.DIRECTORY_ENTRY_BASERELOC for e in block.entries if e.type == 3}
lines = ['// Generated from the original local engine by make_display_profile.py.',
         '#pragma once', 'struct DisplayGuard {DWORD rva; unsigned length; const BYTE* bytes; const unsigned* relocations; unsigned relocationCount;};']
records = []
for i, (rva, length) in enumerate(spans):
    data = image.pe.get_data(rva, length)
    assert len(data) == length
    offsets = sorted(v - rva for v in relocations if rva <= v < rva + length)
    assert all(v + 4 <= length for v in offsets)
    lines.append(f'static const BYTE displayBytes{i}[]={{' + ','.join(f'0x{x:02x}' for x in data) + '};')
    lines.append(f'static const unsigned displayRelocs{i}[]={{' + ','.join(str(v) for v in offsets or [0]) + '};')
    records.append(f'{{0x{rva:x},{length},displayBytes{i},displayRelocs{i},{len(offsets)}}}')
lines.append('static const DisplayGuard displayGuards[]={' + ',\n'.join(records) + '};')
(root / 'src/display_guards.h').write_text('\n'.join(lines) + '\n', encoding='utf-8')
functions = [('display_width',0x2e0240,14), ('display_height',0x2e0260,14),
             ('display_count_context',0x2e0280,64), ('display_current_index',0x2e1050,72),
             ('display_whitelist',0x616ee0,60), ('display_support',0x2e0290,85),
             ('display_setting',0x2e3c70,657), ('display_default',0x4dfa90,782),
             ('display_native_enumeration',0x617560,193), ('display_native_support',0x616f20,87),
             ('display_allocator',0x7def70,32), ('display_free_context',0x7defe0,32),
             ('display_constructor',0x617a30,445), ('display_destructor',0x617bf0,200),
             ('display_refresh_select',0x6180b0,412), ('display_resolution_set',0x618250,111),
             ('display_aa_support',0x2e10b0,116), ('display_aa_native_support',0x616580,104),
             ('display_output_aspect',0x2e0200,47), ('timing_scheduler',0x838ed0,541),
             ('timing_simulation_prepare',0x897b70,270), ('timing_render_dispatch',0x849190,651),
             ('timing_simulation_callback',0x8490e0,42),
             ('display_menu_action_loader',0x9d75c0,171),
             ('display_menu_action_object',0x9d73c0,123),
             ('display_menu_action_reader',0x9d5740,489),
             ('display_device_create_reset',0x6182c0,486),
             ('display_ui_action_resize',0x980be0,111),
             ('display_device_recovery',0x616bb0,211),
             ('display_loading_active',0x4aa7b0,56),
             ('display_loading_registration',0x4abf3d,50),
             ('display_ui_list',0x885e50,145),
             ('display_ui_begin',0x95d080,95),
             ('display_external_interface',0x873100,278),
             ('display_needs_reset',0x616540,57),
             ('display_recovery_dispatch',0x622980,47),
             ('display_present',0x6162d0,75),
             ('system_thread_constructor',0x644fb0,306),
             ('system_wave_output_open',0xab7e30,583),
             ('physics_integrate_kernel',0x6bd8c0,0xe44),('physics_integrate_dispatch',0x6be710,0x142),
             ('system_input_poll',0x642da0,0x150),
             ('display_native_window_proc',0x70ed50,0x40),('display_native_client_size',0x615fa0,0x3b),
             ('display_native_vsync_setter',0x6165f0,0x25),('display_native_aa_setter',0x616620,0x36),
             ('display_text_query',0x873f30,0x26),('display_text_format',0x883920,0xb3)]
# The external-interface slot is data, not an executable function. Its exact
# pointer/relocation remains in displayGuards above; the callee is fingerprinted
# as display_external_interface. Do not feed a four-byte vtable entry into the
# installer's unique executable-function scanner.
profile = json.loads((root / 'src/profile.json').read_text(encoding='utf-8-sig'))
profile['functions'] = [f for f in profile['functions'] if not f['name'].startswith(('display_','timing_','system_','physics_'))]
for name, rva, length in functions:
    data = image.pe.get_data(rva, length)
    profile['functions'].append(dict(name=name, rva=rva, length=length, rvaLocked=True,
        prefix=data[:24].hex(), sha256=hashlib.sha256(data).hexdigest()))
profile['version'] = '0.3.3'
(root / 'src/profile.json').write_text(json.dumps(profile, indent=2) + '\n', encoding='utf-8')
print(json.dumps(dict(guardSpans=len(spans),relocations=sum(1 for rva,n in spans for v in relocations if rva<=v<rva+n),functions=len(functions),gameExecuted=False)))
