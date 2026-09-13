"""Modal build: combined STATIC (non-PIE) binary (elf_loader + gbsh).

Builds a single bionic static executable containing both the ELF loader
and the gbsh shell.  Uses -static (non-PIE) because -static-pie crashes
on this device (kernel 4.14).

Usage: modal run gbsh_combined_static_build.py
Output:
  /tmp/gbsh_combined_static     combined static binary
  ../files/usr/bin/gbsh         deploy to device (app files)
"""

import os
import modal

app = modal.App("gbsh-combined-static-build")
NDK_DIR = "/opt/android-ndk-r28"
TC = f"{NDK_DIR}/toolchains/llvm/prebuilt/linux-x86_64/bin"
DEPLOY = "/root/elf_loader/files/usr/bin/gbsh"

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
    import subprocess
    log = []

    def cc(*args):
        cmd = [f"{TC}/aarch64-linux-android24-clang", "-O1", "-Wall", *args]
        r = subprocess.run(cmd, capture_output=True, text=True)
        log.append(f"$ {' '.join(cmd)}\n{r.stdout}{r.stderr}")
        if r.returncode != 0:
            raise RuntimeError(f"build failed: {r.stderr}")
        return r

    # 1) Create dispatcher main that routes to loader or gbsh
    with open("/tmp/dispatcher.c", "w") as f:
        f.write('''
#include <string.h>
extern int elf_loader_main(int argc, char **argv, char **envp);
extern int gbsh_main(int argc, char **argv);

int main(int argc, char **argv, char **envp) {
    if (argc >= 2) {
        const char *a1 = argv[1];
        int loader_flag = (
            strcmp(a1, "--ownall") == 0 || strcmp(a1, "--shim") == 0 ||
            strcmp(a1, "--run") == 0 || strcmp(a1, "--own") == 0 ||
            strcmp(a1, "--check") == 0 || strcmp(a1, "--lazy") == 0 ||
            strcmp(a1, "--help") == 0 || strcmp(a1, "-h") == 0 ||
            strcmp(a1, "--version") == 0 || strcmp(a1, "-V") == 0);
        if (loader_flag)
            return elf_loader_main(argc, argv, envp);
    }
    return gbsh_main(argc, argv);
}
''')

    # 2) Compile elf_loader sources with main renamed
    cc("-fPIE", "-c", "/src/src/main.c", "-o", "/tmp/main.o", "-Dmain=elf_loader_main")
    cc("-fPIE", "-c", "/src/src/elf_loader.c", "-o", "/tmp/elf_loader.o")
    cc("-c", "/src/src/entry.S", "-o", "/tmp/entry.o")
    cc("-c", "/src/src/dlfcn_stubs.c", "-o", "/tmp/dlfcn_stubs.o")

    # 3) Compile gbsh with main renamed
    cc("-fPIE", "-Dmain=gbsh_main", "-c", "/src/gbsh/gbsh.c", "-o", "/tmp/gbsh.o")

    # 4) Compile dispatcher
    cc("-c", "/tmp/dispatcher.c", "-o", "/tmp/dispatcher.o")

    # 5) Link everything as static (non-PIE)
    cc("-static",
       "/tmp/dispatcher.o", "/tmp/main.o", "/tmp/elf_loader.o", "/tmp/entry.o",
       "/tmp/dlfcn_stubs.o", "/tmp/gbsh.o",
       "-o", "/tmp/gbsh_combined_static")

    # 6) Read back artifact
    out = {}
    name = "gbsh_combined_static"
    with open(f"/tmp/{name}", "rb") as f:
        out[name] = f.read()
    log.append(f"built {name}: {len(out[name])} bytes")

    return {"log": "\n".join(log), "files": out}


@app.local_entrypoint()
def main():
    os.makedirs(os.path.dirname(DEPLOY), exist_ok=True)
    res = build.remote()
    print(res["log"])
    for name, blob in res["files"].items():
        path = f"/tmp/{name}"
        with open(path, "wb") as f:
            f.write(blob)
        with open(DEPLOY, "wb") as f:
            f.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")
        print(f"deployed -> {DEPLOY}")
