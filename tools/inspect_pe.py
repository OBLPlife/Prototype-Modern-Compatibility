"""Read-only PE inspection of the user's original Prototype installation.

Uses generic pefile/capstone packages already present in the workspace.
Does not load or execute the inspected image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

WORKSPACE = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(WORKSPACE / "vendor" / "inspection_python"))
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32


class Image:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        self.pe = pefile.PE(data=self.data)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)

    def refs(self, address):
        pattern = struct.pack("<I", address)
        for section in self.pe.sections:
            data = section.get_data()
            start = 0
            while (offset := data.find(pattern, start)) >= 0:
                yield self.base + section.VirtualAddress + offset
                start = offset + 1

    def disasm(self, address, size):
        offset = self.pe.get_offset_from_rva(address - self.base)
        for ins in self.md.disasm(self.data[offset:offset + size], address):
            yield f"{ins.address:08x}  {ins.bytes.hex(' '):24} {ins.mnemonic:8} {ins.op_str}"

    def imports(self):
        return [{"dll": d.dll.decode(), "functions": [
            {"name": i.name.decode() if i.name else f"ordinal:{i.ordinal}",
             "va": hex(i.address), "refs": [hex(a) for a in self.refs(i.address)]}
            for i in d.imports]} for d in self.pe.DIRECTORY_ENTRY_IMPORT]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--imports", action="store_true")
    parser.add_argument("--refs", nargs="*", type=lambda v: int(v, 0))
    parser.add_argument("--disasm", nargs=2, type=lambda v: int(v, 0))
    parser.add_argument("--strings", metavar="REGEX")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    image = Image(args.image)
    lines = []
    if args.imports:
        report = {"file": args.image.name, "sha256": hashlib.sha256(image.data).hexdigest(),
                  "image_base": hex(image.base), "entry_rva": hex(image.pe.OPTIONAL_HEADER.AddressOfEntryPoint),
                  "machine": hex(image.pe.FILE_HEADER.Machine), "imports": image.imports()}
        lines.append(json.dumps(report, indent=2))
    if args.refs is not None:
        for address in args.refs:
            lines.append(f"{address:08x}: " + ", ".join(f"{a:08x}" for a in image.refs(address)))
    if args.disasm:
        lines.extend(image.disasm(*args.disasm))
    if args.strings:
        for match in re.finditer(rb"[\x20-\x7e]{5,}", image.data):
            value = match.group().decode()
            if re.search(args.strings, value, re.I):
                rva = image.pe.get_rva_from_offset(match.start())
                lines.append(f"{image.base + rva:08x}  {value}")
    output = "\n".join(lines)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output + "\n", encoding="utf-8")
        print(args.output)
    else:
        print(output)


if __name__ == "__main__":
    main()
