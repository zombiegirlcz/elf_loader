"""Modal build: gbsh — bionic shell (dynamická + static-pie varianta).

Usage: modal run gbsh_build.py
Výstup:
  /tmp/gbsh              dynamická bionic (PT_INTERP /system/bin/linker64)
  /tmp/gbsh_static       static-pie, self-contained (doporučeno pro deploy)
"""

import modal

app = modal.App("gbsh-build")
NDK_DIR = "/opt/android-ndk-r28"
TC = f"{NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"

image = (
    modal.Image.debian_slim()
    .apt_install("unzip", "wget")
    .run_commands(
        "wget -q https://dl.google.com/android/repository/android-ndk-r28-linux.zip -O /tmp/ndk.zip",
        "unzip -q /tmp/ndk.zip -d /opt/",
    )
    .add_local_dir("/root/elf_loader", "/src", copy=True)
)


@app.function(image=image, timeout=300)
def build():
    import os
    import subprocess
    log = []

    def cc(*args):
        cmd = [f"{TC}/aarch64-linux-android24-clang", "-O1", "-Wall", *args]
        r = subprocess.run(cmd, capture_output=True, text=True)
        log.append(f"$ {' '.join(cmd)}\n{r.stdout}{r.stderr}")
        if r.returncode != 0:
            raise RuntimeError(f"build failed: {r.stderr}")
        return r

    src = "/src/gbsh/gbsh.c"

    # dynamická bionic (menší, potřebuje /system/bin/linker64 — na device OK)
    cc(src, "-o", "/tmp/gbsh")

    # static-pie (self-contained, žádné NEEDED — jde spustit odkudkoli)
    cc("-fPIE", "-static-pie", "-DNO_DLOPEN", src, "-o", "/tmp/gbsh_static")

    out = {}
    for name in ("gbsh", "gbsh_static"):
        with open(f"/tmp/{name}", "rb") as f:
            out[name] = f.read()
        log.append(f"built {name}: {len(out[name])} bytes")
    return {"log": "\n".join(log), "files": out}


@app.local_entrypoint()
def main():
    res = build.remote()
    print(res["log"])
    for name, blob in res["files"].items():
        with open(f"/tmp/{name}", "wb") as f:
            f.write(blob)
        print(f"wrote /tmp/{name} ({len(blob)} bytes)")
