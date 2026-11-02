#!/usr/bin/env python3
"""Build Xenosaga Episode I: Recompiled from the player's own disc image.

This package ships no game code. This script takes your Xenosaga Episode I
(USA, SLUS-20469) disc image and builds the PC version on your machine:

  1. check the disc and copy the game program and overlays out of it
  2. download PS2Recomp (the open-source recompiler) and apply the port's patch
  3. build the recompiler tools
  4. recompile the game program into C++
  5. build the game (the long step: 15-30 minutes)
  6. copy the finished game into the output folder
  7. tune for speed: the game plays its opening by itself for a few minutes
     while it notes the graphics (VU1) routines it runs, then fast versions of
     them are built in. Roughly doubles the frame rate of a fresh build.

Each step is skipped when it is already done, so an interrupted build picks up
where it stopped. Run it from MSYS2's MINGW64 environment (the launcher does).

Usage:
  python tools/build_from_iso.py --iso <disc.iso> [--work DIR] [--out DIR] [--no-tune]
  python tools/build_from_iso.py --optimize
        rebuild with the graphics routines the game noted while you played
        (<out>/vu1_blocks.vbl; kept in <work>/vu1blocks so none are lost)

Progress lines start with "@@" for the launcher: "@@STEP n/m text",
"@@DONE", "@@ERROR text".
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
PS2RECOMP_URL = "https://github.com/ran-j/PS2Recomp.git"
PS2RECOMP_COMMIT = "e563698"
# SHA-1 of SLUS_204.69 on the retail USA disc (Redump "Xenosaga Episode I -
# Der Wille zur Macht (USA)"). Only a fingerprint; no game data.
SLUS_SHA1 = "fd206d5715a322830f7fa9285fb4a09276ac2a63"
GAME_FILES = ["SYSTEM.CNF", "SLUS_204.69", "OV01.OVL", "OV02.OVL", "OV10.OVL", "OV11.OVL", "OV12.OVL"]
TUNE_SCRIPT = PORT / "inputs" / "newgame-to-tutorial.txt"  # controller script: new game to the tutorial
TUNE_SECONDS = 210
SECTOR = 2048
total_steps = 7


def step(n, text):
    print(f"@@STEP {n}/{total_steps} {text}", flush=True)


def fail(text):
    print(f"@@ERROR {text}", flush=True)
    sys.exit(1)


def run(cmd, cwd=None, env=None):
    print("$ " + " ".join(str(c) for c in cmd), flush=True)
    result = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env)
    if result.returncode != 0:
        fail(f"command failed ({result.returncode}): {Path(str(cmd[0])).name}")


# --- ISO 9660 ------------------------------------------------------------------

def iso_root_files(f):
    """Root directory of an ISO 9660 image: {NAME: (sector, size)}."""
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[1:6] != b"CD001":
        fail("this file is not a disc image (no ISO 9660 header)")
    root = pvd[156:190]
    extent, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    f.seek(extent * SECTOR)
    data = f.read(size)
    files = {}
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:  # records don't cross sectors; skip to the next one
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + length]
        name = rec[33:33 + rec[32]].decode("ascii", "replace").split(";")[0]
        files[name.upper()] = (struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0])
        pos += length
    return files


def extract_game_files(iso: Path, dest: Path):
    dest.mkdir(parents=True, exist_ok=True)
    with iso.open("rb") as f:
        files = iso_root_files(f)
        missing = [n for n in GAME_FILES if n not in files]
        if missing:
            fail("this disc is missing " + ", ".join(missing) + " - is it Xenosaga Episode I (USA)?")
        for name in GAME_FILES:
            sector, size = files[name]
            f.seek(sector * SECTOR)
            (dest / name).write_bytes(f.read(size))
    digest = hashlib.sha1((dest / "SLUS_204.69").read_bytes()).hexdigest()
    if digest != SLUS_SHA1:
        fail("this is not the USA release of Xenosaga Episode I (SLUS-20469), or the image is damaged "
             f"(fingerprint {digest})")
    print("disc checked: Xenosaga Episode I (USA)", flush=True)


# --- build ---------------------------------------------------------------------

class Build:
    def __init__(self, args):
        self.args = args
        self.work = Path(args.work).resolve()
        self.out = Path(args.out).resolve()
        self.msys = Path(os.environ.get("MSYS2_ROOT", r"C:\msys64"))
        env = dict(os.environ)
        env["PATH"] = os.pathsep.join([str(self.msys / "mingw64" / "bin"), str(self.msys / "usr" / "bin"), env.get("PATH", "")])
        self.ps2recomp = self.work / "PS2Recomp"
        env["XS_ROOT"] = str(self.work)
        env["XS_PORT"] = str(PORT)
        env["PS2RECOMP"] = str(self.ps2recomp)
        self.env = env
        self.jobs = ["-j", str(args.jobs)] if args.jobs else []
        self.py = sys.executable
        self.gamefiles = self.work / "gamefiles"
        self.tools_build = self.ps2recomp / "build-tools"
        self.runtime_build = self.ps2recomp / "build-runtime"
        self.blocks_dir = self.work / "vu1blocks"

    def check_tools(self):
        for tool in ("gcc", "cmake", "ninja", "git"):
            if not shutil.which(tool, path=self.env["PATH"]):
                fail(f"the build tools are not installed ({tool} missing); use the launcher's Install button")

    def disc(self):
        step(1, "Checking your disc and copying the game files")
        if not self.args.iso:
            if (self.gamefiles / "SLUS_204.69").exists():
                print("already done", flush=True)
                return
            fail("no disc image given")
        extract_game_files(Path(self.args.iso), self.gamefiles)

    def recompiler(self):
        step(2, "Downloading the recompiler (PS2Recomp)")
        stamp = self.ps2recomp / ".xs_patched"
        if stamp.exists():
            print("already done", flush=True)
            return
        if self.ps2recomp.exists():
            shutil.rmtree(self.ps2recomp)  # a half-finished download of ours; fetched again
        run(["git", "clone", "--quiet", PS2RECOMP_URL, self.ps2recomp], env=self.env)
        run(["git", "-c", "advice.detachedHead=false", "checkout", "--quiet", PS2RECOMP_COMMIT],
            cwd=self.ps2recomp, env=self.env)
        run(["git", "apply", "--whitespace=nowarn", PORT / "patches" / "ps2recomp-runtime.patch"],
            cwd=self.ps2recomp, env=self.env)
        stamp.write_text("ok\n")

    def recompiler_tools(self):
        step(3, "Building the recompiler tools")
        if (self.tools_build / "ps2xRecomp" / "ps2_recomp.exe").exists() and (self.tools_build / "ps2xAnalyzer").exists():
            print("already done", flush=True)
            return
        run(["cmake", "-S", self.ps2recomp, "-B", self.tools_build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF"], env=self.env)
        run(["cmake", "--build", self.tools_build, "--target", "ps2_recomp", "ps2_analyzer", *self.jobs], env=self.env)

    def recompile(self):
        step(4, "Recompiling the game program")
        if (self.work / "recomp" / "output" / "register_functions.cpp").exists():
            print("already done", flush=True)
        else:
            run([self.py, PORT / "tools" / "setup_recomp.py", self.gamefiles, "--bin", self.tools_build], env=self.env)
        self.assemble()

    def assemble(self):
        # Fast graphics routines collected so far (see tune/optimize).
        logs = sorted(self.blocks_dir.glob("*.vbl")) if self.blocks_dir.exists() else []
        if logs:
            run([self.py, PORT / "tools" / "vu1gen.py", *logs], env=self.env)
        run([self.py, PORT / "tools" / "assemble.py"], env=self.env)

    def compile_game(self, n=5, text="Building the game (15-30 minutes; it is fine to leave this running)"):
        step(n, text)
        if not (self.runtime_build / "build.ninja").exists():
            run(["cmake", "-S", self.ps2recomp, "-B", self.runtime_build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 "-DPS2X_BUILD_RECOMP=OFF", "-DPS2X_BUILD_ANALYZER=OFF", "-DPS2X_BUILD_TEST=OFF",
                 "-DPS2X_BUILD_STUDIO=OFF", "-DPS2X_DISABLE_IPO=ON", "-DPS2X_FFMPEG_USE_PKGCONFIG=ON",
                 "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF", "-DPS2X_SHOW_WINDOWS_CONSOLE=OFF",
                 "-DCMAKE_CXX_FLAGS=-msse4.2 -mavx2 -Wa,-mbig-obj"], env=self.env)
        else:
            run(["cmake", self.runtime_build], env=self.env)  # notices a new graphics routines file
        run(["cmake", "--build", self.runtime_build, "--target", "ps2EntryRunner", *self.jobs], env=self.env)

    def install(self, n=6):
        step(n, "Copying the finished game")
        exe = self.runtime_build / "ps2xRuntime" / "ps2EntryRunner.exe"
        if not exe.exists():
            fail("the build finished without producing the game program")
        self.out.mkdir(parents=True, exist_ok=True)
        target = self.out / "XenosagaRecomp.exe"
        if target.exists():
            target.replace(self.out / "XenosagaRecomp.exe.previous")
        shutil.copy2(exe, target)
        shutil.copy2(self.gamefiles / "SLUS_204.69", self.out / "SLUS_204.69")
        copy_dlls(target, self.msys / "mingw64" / "bin", self.out, self.env)

    def collect_blocks(self):
        """Keep the routines the game noted (it only notes ones not built in yet)."""
        vbl = self.out / "vu1_blocks.vbl"
        if not vbl.exists() or vbl.stat().st_size == 0:
            return False
        self.blocks_dir.mkdir(parents=True, exist_ok=True)
        vbl.replace(self.blocks_dir / time.strftime("blocks_%Y%m%d_%H%M%S.vbl"))
        return True

    def tune(self):
        step(7, "Tuning for speed: the game plays its opening by itself for about 4 minutes "
                "(you can watch; please don't press anything)")
        if not self.args.iso:
            fail("tuning needs the disc image")
        env = dict(self.env)
        env["XS_DISC_IMAGE"] = str(Path(self.args.iso).resolve())
        env["XS_NO_SOUND"] = "0"
        env["PS2X_VU1_BLOCKLOG"] = str(self.out / "vu1_blocks.vbl")
        env["PS2X_INPUT_SCRIPT"] = str(TUNE_SCRIPT)
        with open(self.out / "tuning.log", "w") as log:
            game = subprocess.Popen([str(self.out / "XenosagaRecomp.exe"), str(self.out / "SLUS_204.69")],
                                    cwd=self.out, env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.time() + TUNE_SECONDS
            while time.time() < deadline and game.poll() is None:
                time.sleep(1)
            if game.poll() is None:
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(game.pid)], capture_output=True)
                game.wait()
        if not self.collect_blocks():
            print("the game noted no graphics routines; skipping", flush=True)
            return
        self.assemble()
        global total_steps
        self.compile_game(7, "Tuning for speed: building the fast routines in (a few minutes)")
        self.install(7)

    def optimize(self):
        global total_steps
        total_steps = 3
        step(1, "Collecting the graphics routines the game noted while you played")
        if not self.collect_blocks() and not (self.blocks_dir.exists() and any(self.blocks_dir.glob("*.vbl"))):
            fail("nothing to optimize yet: play the game for a while first")
        self.assemble()
        self.compile_game(2, "Building the fast routines in (a few minutes)")
        self.install(3)


def copy_dlls(exe: Path, mingw_bin: Path, out: Path, env):
    """Copy every MSYS2 DLL the game imports, following their imports too."""
    seen, queue, copied = set(), [exe], 0
    objdump = shutil.which("objdump", path=env["PATH"])
    while queue:
        f = queue.pop()
        listing = subprocess.run([objdump, "-p", str(f)], capture_output=True, text=True, env=env).stdout
        for line in listing.splitlines():
            if "DLL Name:" not in line:
                continue
            name = line.split("DLL Name:")[1].strip()
            if name.lower() in seen:
                continue
            seen.add(name.lower())
            src = mingw_bin / name
            if src.exists():
                shutil.copy2(src, out / name)
                queue.append(src)
                copied += 1
    print(f"copied {copied} libraries", flush=True)


def main():
    global total_steps
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", help="your Xenosaga Episode I (USA) disc image")
    ap.add_argument("--work", default=str(PORT / "work"), help="build folder (several GB)")
    ap.add_argument("--out", default=str(PORT / "game"), help="where the finished game goes")
    ap.add_argument("--jobs", type=int, default=0, help="parallel compile jobs (default: all cores)")
    ap.add_argument("--no-tune", action="store_true", help="skip the speed tuning run after the first build")
    ap.add_argument("--optimize", action="store_true",
                    help="build in the graphics routines the game noted while you played")
    args = ap.parse_args()

    # Build at low priority (the compilers inherit it): the PC stays usable,
    # and a game played meanwhile isn't starved of CPU.
    if os.name == "nt":
        import ctypes
        BELOW_NORMAL_PRIORITY_CLASS = 0x4000
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)

    build = Build(args)
    build.check_tools()
    if args.optimize:
        build.optimize()
    else:
        first_build = not (build.out / "XenosagaRecomp.exe").exists()
        tune = first_build and not args.no_tune
        total_steps = 7 if tune else 6
        build.disc()
        build.recompiler()
        build.recompiler_tools()
        build.recompile()
        build.compile_game()
        build.install()
        if tune:
            build.tune()
    print("@@DONE", flush=True)


if __name__ == "__main__":
    main()
