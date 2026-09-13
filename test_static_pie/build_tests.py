"""Modal build: static-pie test cases to isolate crash cause.

Builds minimal test programs with increasing complexity to identify
what causes static-pie to crash on this device.

Usage: modal run test_static_pie/build_tests.py
Output: /tmp/test_static_pie_<name> for each test case
"""

import os
import subprocess
import modal

app = modal.App("test-static-pie")
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

    def run(cmd):
        r = subprocess.run(cmd, capture_output=True, text=True, shell=True)
        log.append(f"$ {cmd}\n{r.stdout}{r.stderr}")
        if r.returncode != 0:
            log.append(f"EXIT CODE: {r.returncode}")
        return r

    tests = {
        "01_minimal": "/src/tests/minimal.c",
        "02_exit_code": "/src/tests/exit_code.c",
        "03_write": "/src/tests/write.c",
        "04_malloc": "/src/tests/malloc.c",
        "05_string": "/src/tests/string.c",
        "06_env": "/src/tests/env.c",
        "07_fork": "/src/tests/fork.c",
    }

    for name, src in tests.items():
        out = f"/tmp/test_static_pie_{name}"
        run(f"{cc} -O1 -Wall -fPIE -static-pie {src} -o {out}")

    out = {}
    for name in tests:
        path = f"/tmp/test_static_pie_{name}"
        if os.path.exists(path):
            with open(path, "rb") as f:
                out[name] = f.read()
            log.append(f"built {name}: {len(out[name])} bytes")
        else:
            log.append(f"MISSING {name}")

    return {"log": "\n".join(log), "files": out}


@app.local_entrypoint()
def main():
    os.makedirs(DEPLOY_DIR, exist_ok=True)
    res = build.remote()
    print(res["log"])
    for name, blob in res["files"].items():
        path = f"/tmp/test_static_pie_{name}"
        with open(path, "wb") as f:
            f.write(blob)
        deploy = os.path.join(DEPLOY_DIR, name)
        with open(deploy, "wb") as f:
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes) -> {deploy}")
