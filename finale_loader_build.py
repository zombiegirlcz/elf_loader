"""Modal build: NDK cross-compile the whole elf_loader (bug.md Task 2).

Packs src/main.c + src/elf_loader.c + src/entry.S + include/*.h, compiles with
aarch64-linux-android24-clang (same flags as Makefile minus the proot-only
-B/usr/bin workaround). Builds two variants:

  /tmp/elf_loader_ndk              default dynamic (bionic libc)  -> adb push
  /tmp/elf_loader_ndk_staticpie    -static-pie (self-contained)   -> runs here

Also greps the bionic sysroot for MAP_FIXED_NOREPLACE availability/guard.

Usage: modal run finale_loader_build.py
"""

import modal

app = modal.App("elf-loader-ndk")

NDK_VERSION = "r28"
NDK_DIR = f"/opt/android-ndk-{NDK_VERSION}"
TC = f"{NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"

image = (
    modal.Image.debian_slim()
    .apt_install("unzip", "wget")
    .run_commands(
        f"wget -q https://dl.google.com/android/repository/android-ndk-{NDK_VERSION}-linux.zip -O /tmp/ndk.zip",
        f"unzip -q /tmp/ndk.zip -d /opt/",
        f"test -x {TC}/aarch64-linux-android24-clang",
    )
    .add_local_dir("/root/elf_loader", "/src",
                   ignore=modal.FilePatternMatcher(".git", "*.o", "elf_loader",
                                                   "test", "*.py", "*.md"))
)


@app.function(image=image)
def build():
    import os
    import re
    import subprocess

    log = []

    def run(cmd, **kw):
        r = subprocess.run(cmd, capture_output=True, text=True, **kw)
        log.append(f"$ {' '.join(cmd)}")
        log.append(r.stdout)
        log.append(r.stderr)
        return r

    # 2a) MAP_FIXED_NOREPLACE availability in the bionic sysroot
    sysroot = f"{NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
    mman = f"{sysroot}/usr/include/sys/mman.h"
    if os.path.exists(mman):
        with open(mman) as f:
            txt = f.read()
        log.append("-- mman.h context --")
        for line in txt.splitlines():
            if "MAP_FIXED_NOREPLACE" in line:
                log.append(line.rstrip())
        for api in ("24", "29", "30", "35"):
            r = subprocess.run(
                [f"{TC}/aarch64-linux-android24-clang", "-dM", "-E",
                 "-D__ANDROID_API__=" + api, mman],
                capture_output=True, text=True)
            has = "MAP_FIXED_NOREPLACE" in r.stdout
            val = ""
            for l in r.stdout.splitlines():
                if "MAP_FIXED_NOREPLACE" in l:
                    val = l.strip()
            log.append(f"API {api}: MAP_FIXED_NOREPLACE={'YES ' + val if has else 'NO'}")
        with open(mman) as f:
            guard = None
            for i, line in enumerate(f.read().splitlines()):
                if "__ANDROID_API__" in line and "FIXED_NOREPLACE" in line:
                    guard = line.strip()
            log.append(f"guard line: {guard}")

    os.chdir("/src")

    flags = ["-Wall", "-Wextra", "-g", "-O0", "-std=c11"]
    srcs = ["src/main.c", "src/elf_loader.c", "src/entry.S"]

    # dynamic variant (default bionic link) -> adb push target
    run([f"{TC}/aarch64-linux-android24-clang", *flags, *srcs,
         "-ldl", "-o", "/tmp/elf_loader_ndk"])

    # static-PIE variant (self-contained, runnable on this host).
    # bionic static libc has no dlfcn (no dynamic linker) -> link fails unless
    # we supply no-op stubs (logged: missing symbols dlopen/dlsym/dlclose/dladdr).
    run([f"{TC}/aarch64-linux-android24-clang", *flags, "-fPIE",
         "-static-pie", *srcs, "src/dlfcn_stubs.c",
         "-o", "/tmp/elf_loader_ndk_staticpie"])

    out = {}
    for name in ("elf_loader_ndk", "elf_loader_ndk_staticpie"):
        p = f"/tmp/{name}"
        if os.path.exists(p):
            with open(p, "rb") as f:
                out[name] = f.read()
            log.append(f"built {name}: {len(out[name])} bytes")
        else:
            log.append(f"MISSING {name}")

    return {"log": "\n".join(log), "files": out}


@app.local_entrypoint()
def main():
    res = build.remote()
    print(res["log"])
    for name, blob in res["files"].items():
        with open(f"/tmp/{name}", "wb") as f:
            f.write(blob)
        print(f"wrote /tmp/{name} ({len(blob)} bytes)")