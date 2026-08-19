"""Modal build: cross-compile Android (bionic) binaries with the NDK.

Usage: modal run finale_build.py
Writes /tmp/bstatic (fully-static bionic, ET_EXEC) and
        /tmp/bstaticpie (bionic static-PIE, ET_DYN) locally.
Then run locally through the own loader:
        ./elf_loader --run /tmp/bstatic
        ./elf_loader --run /tmp/bstaticpie
"""

import modal

app = modal.App("elf-loader-finale")

NDK_VERSION = "r28"
NDK_DIR = f"/opt/android-ndk-{NDK_VERSION}"

image = (
    modal.Image.debian_slim()
    .apt_install("unzip", "wget")
    .run_commands(
        f"wget -q https://dl.google.com/android/repository/android-ndk-{NDK_VERSION}-linux.zip -O /tmp/ndk.zip",
        f"unzip -q /tmp/ndk.zip -d /opt/",
        f"test -x {NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang",
    )
)

SRC = r'''#include <stdio.h>
#include <unistd.h>
#include <string.h>
static const char msg[] = "bionic (NDK) hello\n";
int main(void) {
    write(1, msg, sizeof(msg) - 1);
    printf("printf via bionic: %d\n", 42);
    return 42;
}
'''


@app.function(image=image)
def build():
    import os
    import subprocess

    tc = f"{NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"
    cc = f"{tc}/aarch64-linux-android24-clang"

    with open("/tmp/hello.c", "w") as f:
        f.write(SRC)

    out = {}
    variants = {
        "bstatic": ["-static"],
        "bstaticpie": ["-static-pie", "-fPIE", "-pie"],
    }
    for name, flags in variants.items():
        subprocess.run(
            [cc, *flags, "/tmp/hello.c", "-o", f"/tmp/{name}"], check=True
        )
        with open(f"/tmp/{name}", "rb") as f:
            out[name] = f.read()
        print(f"built {name}: {len(out[name])} bytes")

    return out


@app.local_entrypoint()
def main():
    data = build.remote()
    for name, blob in data.items():
        with open(f"/tmp/{name}", "wb") as f:
            f.write(blob)
        print(f"wrote /tmp/{name} ({len(blob)} bytes)")