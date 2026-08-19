"""Modal build: cross-compile elf_loader for Android (bionic) with NDK.

Usage: modal run build_ndk.py
Writes /tmp/elf_loader_ndk locally (PIE, linked against bionic libc.so + libdl.so).
"""

import modal

app = modal.App("elf-loader-ndk-build")

NDK_VERSION = "r28"
NDK_DIR = f"/opt/android-ndk-{NDK_VERSION}"

# Build image with source code included
image = (
    modal.Image.debian_slim()
    .apt_install("unzip", "wget")
    .run_commands(
        f"wget -q https://dl.google.com/android/repository/android-ndk-{NDK_VERSION}-linux.zip -O /tmp/ndk.zip",
        f"unzip -q /tmp/ndk.zip -d /opt/",
        f"test -x {NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang",
    )
    .add_local_dir("/root/elf_loader/src", remote_path="/src", copy=True)
    .add_local_dir("/root/elf_loader/include", remote_path="/include", copy=True)
)

@app.function(image=image, timeout=300)
def build():
    import os
    import subprocess

    tc = f"{NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"
    cc = f"{tc}/aarch64-linux-android24-clang"
    strip = f"{tc}/llvm-strip"

    src_dir = "/src"

    env = os.environ.copy()
    env["CC"] = cc
    env["CFLAGS"] = "-Wall -Wextra -g -O0 -std=c11 -fPIE -I/include"
    env["LDFLAGS"] = "-ldl -pie"

    subprocess.run(
        [cc, "-Wall", "-Wextra", "-g", "-O0", "-std=c11", "-fPIE", "-ldl", "-pie", "-I/include",
         "-o", "/tmp/elf_loader_ndk",
         f"{src_dir}/main.c", f"{src_dir}/elf_loader.c", f"{src_dir}/entry.S"],
        check=True, env=env
    )
    
    subprocess.run([strip, "/tmp/elf_loader_ndk"], check=True)

    with open("/tmp/elf_loader_ndk", "rb") as f:
        return f.read()


@app.local_entrypoint()
def main():
    data = build.remote()
    with open("/tmp/elf_loader_ndk", "wb") as f:
        f.write(data)
    print(f"wrote /tmp/elf_loader_ndk ({len(data)} bytes)")