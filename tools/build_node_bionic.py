"""
Cross-compile Node.js v26 for Android bionic (aarch64) on Modal.

Použití:
    modal run tools/build_node_bionic.py::build

    # stažení výsledku:
    modal volume get node-bionic /out/node ./node-v26-bionic

Poznámky:
- Node oficiálně dropnul Android target po v20; pro v26 to je "best effort".
- android-configure v master pořád existuje, používá NDK sysroot.
- Cílová API 26 (Android 8 — dostatečné pro Android 10+/Xiaomi HyperOS).
- Volume `node-bionic` drží zdrojáky (rychlejší rebuildy) i finální binárku.
"""

from __future__ import annotations
import subprocess
import modal

NODE_VERSION = "v26.10.0"
NDK_VERSION = "r29"  # r27/r28 clang libc++ neumí std::atomic_ref (V8 v26 ho vyžaduje)
ANDROID_API = "28"
ARCH = "arm64"

app = modal.App("node-bionic-build")
vol = modal.Volume.from_name("node-bionic", create_if_missing=True)

image = (
    modal.Image.debian_slim(python_version="3.12")
    .apt_install(
        "curl", "unzip", "xz-utils", "git", "python3", "ccache",
        "build-essential", "pkg-config",
    )
    .env({"CCACHE_DIR": "/vol/ccache"})
)


@app.function(
    image=image,
    volumes={"/vol": vol},
    cpu=16.0,
    memory=32768,
    timeout=60 * 60 * 2,  # 2 hodiny
)
def build() -> str:
    import os
    import shutil
    from pathlib import Path

    vol_path = Path("/vol")
    ndk_dir = vol_path / f"android-ndk-{NDK_VERSION}"
    src_dir = vol_path / f"node-{NODE_VERSION}"
    out_dir = vol_path / "out"
    out_dir.mkdir(exist_ok=True)
    (vol_path / "ccache").mkdir(exist_ok=True)

    def run(cmd, cwd=None, env=None):
        print(f"$ {cmd}", flush=True)
        subprocess.run(cmd, cwd=cwd, env=env, shell=True, check=True)

    # 1) Android NDK
    if not ndk_dir.exists():
        zip_path = vol_path / f"ndk-{NDK_VERSION}.zip"
        if not zip_path.exists():
            run(
                f"curl -fL -o {zip_path} "
                f"https://dl.google.com/android/repository/android-ndk-{NDK_VERSION}-linux.zip"
            )
        run(f"unzip -q {zip_path} -d {vol_path}")
        vol.commit()

    # 2) Node source
    if not src_dir.exists():
        tar_path = vol_path / f"node-{NODE_VERSION}.tar.gz"
        if not tar_path.exists():
            run(
                f"curl -fL -o {tar_path} "
                f"https://nodejs.org/dist/{NODE_VERSION}/node-{NODE_VERSION}.tar.gz"
            )
        run(f"tar -xzf {tar_path} -C {vol_path}")
        vol.commit()

    # 3) Konfigurace pro Android
    #    android-configure v v26 je Python skript, který si sám zavolá ./configure.
    #    Nejprve aplikuj V8 trap-handler patch (`patch` režim), pak configure.
    env = os.environ.copy()
    env["PATH"] = f"/usr/lib/ccache:{env['PATH']}"

    # Patch je idempotentní jen pomocí -N; přeskočíme, pokud už byl aplikován.
    run(f"./android-configure patch || true", cwd=src_dir, env=env)
    run(f"./android-configure {ndk_dir} {ANDROID_API} {ARCH}", cwd=src_dir, env=env)

    # 4) Build
    nproc = os.cpu_count() or 8
    run(f"make -j{nproc} V=0", cwd=src_dir, env=env)

    # 5) Ověření a kopie výsledku
    node_bin = src_dir / "out" / "Release" / "node"
    if not node_bin.exists():
        raise RuntimeError(f"binárka nevznikla: {node_bin}")

    size_mb = node_bin.stat().st_size / (1024 * 1024)
    print(f"node velikost: {size_mb:.1f} MB", flush=True)

    # ověření: bionic interpreter
    file_out = subprocess.check_output(
        f"{ndk_dir}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf -l {node_bin}",
        shell=True, text=True,
    )
    print(file_out[:2000], flush=True)
    if "linker64" not in file_out and "linker" not in file_out:
        print("VAROVÁNÍ: interpreter není bionic linker", flush=True)

    final = out_dir / "node"
    shutil.copy2(node_bin, final)
    vol.commit()

    return f"OK, {size_mb:.1f} MB, /vol/out/node"


@app.local_entrypoint()
def main():
    result = build.remote()
    print(result)
    print("Stáhni: modal volume get node-bionic /out/node ./node-v26-bionic")
