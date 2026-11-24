"""Verify Windows x64 imports and stage an isolated local test build in runs/."""
import argparse
import hashlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parent
SYSTEM = set("bcrypt.dll gdi32.dll kernel32.dll msvcrt.dll opengl32.dll "
             "shell32.dll user32.dll winmm.dll".split())


def inspect_pe(path):
    output = subprocess.check_output(
        ["llvm-readobj", "--coff-imports", "--coff-exports", str(path)], text=True)
    if "Format: COFF-x86-64\n" not in output or "DelayImport {" in output:
        raise ValueError(f"Unsupported PE architecture or delay imports in {path}")
    imports = {}
    for block in re.findall(r"^Import \{(.*?)^\}", output, re.M | re.S):
        name = re.search(r"^  Name: (.+)$", block, re.M).group(1).lower()
        if not re.fullmatch(r"[a-z0-9_.+-]+\.dll", name):
            raise ValueError(f"Invalid DLL name in {path}")
        symbols = re.findall(r"^  Symbol: (.*?) \(\d+\)$", block, re.M)
        if not symbols or len(symbols) != block.count("  Symbol:") or "" in symbols:
            raise ValueError(f"Unsupported import format in {path} for {name}")
        imports[name] = set(symbols)
    exports = set(re.findall(r"^Export \{\n  Ordinal: \d+\n  Name: (.+)$", output, re.M))
    return imports, exports


def dependency_closure(exe, ffmpeg_bin):
    paths = {}
    exports = {}
    pending = [exe]
    seen = set()
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        imports, _ = inspect_pe(path)
        if path == exe and not imports:
            raise ValueError("Expected a dynamically linked runner")
        print(path.name, "imports", ", ".join(sorted(imports)))
        for dll, symbols in imports.items():
            if dll.startswith(("msys-", "cygwin")):
                raise ValueError(f"Unexpected POSIX emulation dependency {dll}")
            if dll in SYSTEM:
                continue
            if dll not in paths:
                candidate = ffmpeg_bin / dll
                if not candidate.is_file():
                    candidate = Path(subprocess.check_output(
                        ["x86_64-w64-mingw32-g++-posix", "-print-file-name=" + dll],
                        text=True).strip())
                    if not candidate.is_absolute() or not candidate.is_file():
                        raise ValueError(f"Unresolved {dll} imported by {path.name}")
                paths[dll] = candidate
                _, exports[dll] = inspect_pe(candidate)
                pending.append(candidate)
            missing = symbols - exports[dll]
            if missing:
                raise ValueError(f"{dll} lacks {sorted(missing)} required by {path.name}")
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("--ffmpeg-bin", type=Path, default=ROOT / "deps/ffmpeg-win64/bin")
    parser.add_argument("--gamefiles", type=Path, default=ROOT / "gamefiles")
    args = parser.parse_args()
    files = dependency_closure(args.exe.resolve(strict=True), args.ffmpeg_bin)
    files["XenosagaRecomp.exe"] = args.exe
    files["Launch Xenosaga.bat"] = PORT / "dist/Launch Xenosaga.bat"
    elf = args.gamefiles / "SLUS_204.69"
    if hashlib.sha1(elf.read_bytes()).hexdigest() != "fd206d5715a322830f7fa9285fb4a09276ac2a63":
        raise ValueError("Unexpected SLUS executable fingerprint")
    files[elf.name] = elf
    runs = ROOT / "runs"
    runs.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=runs) as staging:
        staged = Path(staging)
        manifest = []
        for name, path in sorted(files.items()):
            shutil.copy2(path, staged / name)
            digest = hashlib.sha256((staged / name).read_bytes()).hexdigest()
            manifest.append(f"{digest}  {name}\n")
        contents = "".join(manifest)
        (staged / "SHA256SUMS").write_text(contents)
        target = runs / ("win64-" + hashlib.sha256(contents.encode()).hexdigest()[:16])
        if target.exists():
            for name in [*files, "SHA256SUMS"]:
                if (target / name).read_bytes() != (staged / name).read_bytes():
                    raise ValueError(f"Existing stage differs at {name}")
        else:
            staged.rename(target)
        print(target)


if __name__ == "__main__":
    main()
