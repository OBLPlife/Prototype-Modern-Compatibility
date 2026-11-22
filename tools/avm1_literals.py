"""Make our compiled AS2 extension independent of the game's constant pool.

Only the compiler output of our own scripts is transformed. Function sizes and
relative branches are relocated together; original game bytecode is not input.
Format: Adobe SWF 19, ActionPush / ActionDefineFunction(2) / ActionConstantPool.
"""
import struct


def inline_constants(code):
    actions, at, pool = [], 0, None
    while at < len(code):
        start, op = at, code[at]
        at += 1
        size = 0
        if op >= 0x80:
            size, = struct.unpack_from('<H', code, at)
            at += 2
        data = code[at:at+size]
        assert len(data) == size
        at += size
        # These actions have additional block sizes or action-count operands.
        # Our scripts do not use them; don't silently produce invalid output.
        assert op not in (0x8a, 0x8d, 0x8f, 0x94), hex(op)
        target = body_end = None
        if op == 0x88:
            assert start == 0 and pool is None, 'expected one leading own constant pool'
            count, = struct.unpack_from('<H', data)
            assert data[-1] == 0
            pool = data[2:-1].split(b'\0')
            assert len(pool) == count
            encoded = b''
        else:
            if op == 0x96:
                values, pos = bytearray(), 0
                while pos < len(data):
                    begin, kind = pos, data[pos]
                    pos += 1
                    if kind == 0:
                        pos = data.index(0, pos) + 1
                    else:
                        widths = {1: 4, 2: 0, 3: 0, 4: 1, 5: 1, 6: 8, 7: 4, 8: 1, 9: 2}
                        pos += widths[kind]
                    assert pos <= len(data)
                    if kind in (8, 9):
                        index = int.from_bytes(data[begin+1:pos], 'little')
                        assert pool is not None and index < len(pool)
                        values.extend(b'\0' + pool[index] + b'\0')
                    else:
                        values.extend(data[begin:pos])
                data = bytes(values)
            elif op in (0x99, 0x9d):
                assert len(data) == 2
                target = at + struct.unpack('<h', data)[0]
            elif op in (0x9b, 0x8e):
                pos = data.index(0) + 1
                count, = struct.unpack_from('<H', data, pos)
                pos += 2 + (3 if op == 0x8e else 0)
                for _ in range(count):
                    if op == 0x8e:
                        pos += 1
                    pos = data.index(0, pos) + 1
                assert pos + 2 == len(data), 'function action header excludes its body'
                body_end = at + struct.unpack_from('<H', data, pos)[0]
            encoded = bytes([op]) + (struct.pack('<H', len(data)) + data if op >= 0x80 else b'')
        actions.append((start, at, encoded, target, body_end))
    assert at == len(code) and pool is not None and code[-1] == 0
    offsets, cursor = {}, 0
    for start, _, encoded, _, _ in actions:
        offsets[start] = cursor
        cursor += len(encoded)
    offsets[len(code)] = cursor
    output = bytearray()
    for start, end, encoded, target, body_end in actions:
        if target is not None:
            delta = offsets[target] - (offsets[start] + len(encoded))
            encoded = encoded[:-2] + struct.pack('<h', delta)
        if body_end is not None:
            size = offsets[body_end] - offsets[end]
            encoded = encoded[:-2] + struct.pack('<H', size)
        output.extend(encoded)
    assert len(output) == cursor
    return bytes(output)
