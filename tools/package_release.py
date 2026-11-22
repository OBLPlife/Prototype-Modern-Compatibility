"""Archive the buildable production sources for the Steam Prototype patch."""
import argparse
import hashlib
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = "python-3.13.12-embed-amd64.zip"
PYTHON_HASH = "76f238f606250c87c6beac75dccd35ee99070a13490555936abb6cb64ecce3d0"


def add_entry(archive, name, data):
    entry = ZipInfo("PrototypeModern/" + name, (2026, 11, 22, 0, 0, 0))
    entry.external_attr = 0o644 << 16
    archive.writestr(entry, data, compress_type=ZIP_DEFLATED, compresslevel=9)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_zip", type=Path)
    args = parser.parse_args()
    runtime = ROOT / "vendor" / RUNTIME
    if not runtime.is_file():
        runtime = ROOT / "build" / RUNTIME
    runtime_data = runtime.read_bytes()
    if hashlib.sha256(runtime_data).hexdigest() != PYTHON_HASH:
        raise ValueError("Incorrect embeddable runtime.")
    names = {"build.ps1", "RELEASE.md", "src/compat.cpp", "src/compat.def", "src/startup_bridge.cpp", "src/profile.json"}
    for pattern in ("*.h", "*.as"):
        names.update(p.relative_to(ROOT).as_posix() for p in (ROOT / "src").glob(pattern))
    names.update(p.relative_to(ROOT).as_posix() for p in (ROOT / "installer").glob("*") if p.is_file())
    for tool in ("repair_existing.cpp", "repair_socket_stub.asm", "capture_engine.py", "capture_text.py",
                 "build_ui_actions.py", "avm1_literals.py", "generate_device_forwarders.py",
                 "build_movie_canvas.py", "make_display_profile.py", "inspect_pe.py",
                 "check_release.cs", "check_bridge_installer.cs", "package_release.py"):
        names.add("tools/" + tool)
    out = args.source_zip.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(out, "w") as archive:
        for name in sorted(names):
            data = (ROOT / name).read_bytes()
            if name == "tools/repair_existing.cpp":
                # Only the library body compiled by startup_bridge.cpp belongs to the product.
                # The separate diagnostic CLI, its PID arguments and its fixtures are not shipped.
                src = data.decode("utf-8-sig").split("#ifndef PM_REPAIR_LIBRARY", 1)[0]
                src = src[src.index("#define WIN32_LEAN_AND_MEAN"):]
                data = ("// Production include for PrototypeModern_Start only.\n"
                        "#if !defined(PM_REPAIR_LIBRARY)\n#error Include from startup_bridge.cpp\n#endif\n" + src).encode()
            add_entry(archive, name, data)
        add_entry(archive, "vendor/" + RUNTIME, runtime_data)
    with ZipFile(out) as archive:
        if archive.testzip() or len(archive.namelist()) != len(names) + 1:
            raise ValueError("Invalid source archive.")
    print(out)


if __name__ == "__main__":
    main()

