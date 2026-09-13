"""Compare static vs static-pie vs dynamic on device via ashell."""

import os
import subprocess
import modal

app = modal.App("test-static-compare")
NDK = "/opt/android-ndk-r28"
TC = f"{NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin"
DEPLOY_DIR = "/root/elf_loader/files/usr/bin/test_static_pie"

image = (
    modal.Image.debian_slim()
    .apt_install("unzip", "wget")
    .run_commands(
        "wget -q https://dl.google.com/android/repository/android-ndk-r28-linux.zip -O /tmp/ndk.zip",
        "unzip -q /tmp/ndk.zip -d /opt/",
    )
    .add_local_dir("/root/elf_loader/test_static_pie", "/src/tests", copy=True)
)


@app.function(image=image, timeout=300)
def build():
    log = []
    cc = f"{TC}/aarch64-linux-android24-clang"
    src = "/src/tests/minimal.c"

    variants = {
        "dynamic": f"{cc} -O1 -Wall {src} -o /tmp/test_dynamic",
        "static": f"{cc} -O1 -Wall -static {src} -o /tmp/test_static",
        "static_pie": f"{cc} -O1 -Wall -fPIE -static-pie {src} -o /tmp/test_static_pie",
    }

    for name, cmd in variants.items():
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        log.append(f"$ {cmd}")
        log.append(r.stdout + r.stderr)
        if r.returncode != 0:
            log.append(f"EXIT CODE: {r.returncode}")

    out = {}
    for name in ("dynamic", "static", "static_pie"):
        path = f"/tmp/test_{name}"
        if os.path.exists(path):
            with open(path, "rb") as f:
                out[name] = f.read()
            log.append(f"built test_{name}: {len(out[name])} bytes")
        else:
            log.append(f"MISSING test_{name}")

    return {"log": "\n".join(log), "files": out}


@app.local_entrypoint()
def main():
    os.makedirs(DEPLOY_DIR, exist_ok=True)
    res = build.remote()
    print(res["log"])
    for name, blob in res["files"].items():
        path = f"/tmp/test_{name}"
        with open(path, "wb") as f:
            f.write(blob)
        deploy = os.path.join(DEPLOY_DIR, f"compare_{name}")
        with open(deploy, "wb") as f:
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes) -> {deploy}")
