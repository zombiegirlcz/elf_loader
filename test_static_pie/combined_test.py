"""Test: combined static binary (minimal + gbsh stub)."""

import os
import subprocess
import modal

app = modal.App("test-combined-static")
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

    # Create a minimal combined test: main.c that calls test_func from another file
    with open("/src/tests/combined_main.c", "w") as f:
        f.write('''
extern int test_func(void);
int main(void) { return test_func(); }
''')
    with open("/src/tests/combined_lib.c", "w") as f:
        f.write('''
int test_func(void) { return 0; }
''')

    variants = {
        "combined_dynamic": f"{cc} -O1 -Wall /src/tests/combined_main.c /src/tests/combined_lib.c -o /tmp/test_combined_dynamic",
        "combined_static": f"{cc} -O1 -Wall -static /src/tests/combined_main.c /src/tests/combined_lib.c -o /tmp/test_combined_static",
        "combined_static_pie": f"{cc} -O1 -Wall -fPIE -static-pie /src/tests/combined_main.c /src/tests/combined_lib.c -o /tmp/test_combined_static_pie",
    }

    for name, cmd in variants.items():
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        log.append(f"$ {cmd}")
        log.append(r.stdout + r.stderr)
        if r.returncode != 0:
            log.append(f"EXIT CODE: {r.returncode}")

    out = {}
    for name in ("combined_dynamic", "combined_static", "combined_static_pie"):
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
        deploy = os.path.join(DEPLOY_DIR, name)
        with open(deploy, "wb") as f:
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes) -> {deploy}")
