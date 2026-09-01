#!/bin/bash
set -euo pipefail

ARCH="amd64"
UBUNTU_CODENAME="bionic"
UBUNTU_VERSION="18.04.5"
BASE_URL="https://cdimage.ubuntu.com/ubuntu-base/releases/${UBUNTU_VERSION}/release/ubuntu-base-${UBUNTU_VERSION}-base-${ARCH}.tar.gz"
ROOTFS_DIR="$(cd "$(dirname "$0")/testrootfs" && pwd)"
TMP_TAR="$(mktemp -u).tar.gz"
IMAGE_NAME="proot-bionic:latest"

cleanup() {
  rm -f "$TMP_TAR"
}
trap cleanup EXIT

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "[!] Chybí potřebný příkaz: $1" >&2
    exit 1
  fi
}

require_cmd curl
require_cmd tar

if [ ! -d "$ROOTFS_DIR/bin" ]; then
  echo "[*] Stahuji Ubuntu ${UBUNTU_VERSION} ${ARCH} base rootfs..."
  mkdir -p "$ROOTFS_DIR"
  curl -LOk "$BASE_URL" -o "$TMP_TAR"
  echo "[*] Rozbaluji do: $ROOTFS_DIR"
  tar -xzf "$TMP_TAR" -C "$ROOTFS_DIR"
else
  echo "[*] Rootfs už existuje: $ROOTFS_DIR (přeskakuji rozbalování)"
fi

mkdir -p "$ROOTFS_DIR/parrot"
mkdir -p "$ROOTFS_DIR/bin"

cat > "$ROOTFS_DIR/bin/proot-launch" <<'EOF'
#!/bin/bash
set -e
ROOTFS="<ROOTFS>"
CMD="${1:-/bin/bash -l}"
shift || true
exec proot \
  -r "$ROOTFS" \
  -0 \
  -w / \
  -b /:/parrot \
  -b /dev \
  -b /proc \
  -b /sys \
  $CMD "$@"
EOF

chmod +x "$ROOTFS_DIR/bin/proot-launch"
sed -i "s#<ROOTFS>#${ROOTFS_DIR//#/\\#}#g" "$ROOTFS_DIR/bin/proot-launch"

cat > "$ROOTFS_DIR/bin/resolvconf-gen" <<'EOF'
#!/bin/bash
set -e
mkdir -p /etc
cat > /etc/resolv.conf <<RESOLV
nameserver 8.8.8.8
nameserver 1.1.1.1
RESOLV
EOF
chmod +x "$ROOTFS_DIR/bin/resolvconf-gen"

cat > "$ROOTFS_DIR/init-rootfs.sh" <<'EOF'
#!/bin/bash
set -e
export DEBIAN_FRONTEND=noninteractive
export PATH="/bin:/usr/bin:/sbin:/usr/sbin:$PATH"

/bin/resolvconf-gen

if [ ! -x /usr/bin/apt-get ]; then
  echo "[rootfs] apt-get chybí, opravuji..."
  rm -f /usr/bin/apt-get /usr/bin/dpkg /usr/bin/apt 2>/dev/null || true
  if [ -f /usr/bin/apt-get.orig ]; then
    mv /usr/bin/apt-get.orig /usr/bin/apt-get
    mv /usr/bin/dpkg.orig /usr/bin/dpkg
    mv /usr/bin/apt.orig /usr/bin/apt
  fi
fi

apt-get update || true
apt-get install -y --no-install-recommends \
  bash ca-certificates curl iproute2 net-tools procps \
  vim-tiny less man-db tzdata locales || true

if [ -f /etc/locale.gen ]; then
  sed -i 's/^# *en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen || true
  locale-gen || true
fi

if [ -x /usr/sbin/update-locale ]; then
  update-locale LANG=en_US.UTF-8 || true
fi

if [ -x /usr/bin/apt-get ]; then
  apt-get clean || true
fi
EOF
chmod +x "$ROOTFS_DIR/init-rootfs.sh"

cat > "$ROOTFS_DIR/.hushlogin" || true

cat > "$ROOTFS_DIR/bin/run-test" <<'EOF'
#!/bin/bash
set -e
ROOTFS="<ROOTFS>"
exec proot \
  -r "$ROOTFS" \
  -0 \
  -w / \
  -b /:/parrot \
  -b /dev \
  -b /proc \
  -b /sys \
  /bin/bash -l
EOF
chmod +x "$ROOTFS_DIR/bin/run-test"
sed -i "s#<ROOTFS>#${ROOTFS_DIR//#/\\#}#g" "$ROOTFS_DIR/bin/run-test"

# Docker setup
if command -v docker >/dev/null 2>&1; then
  if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    echo "[*] Building Docker image: $IMAGE_NAME"
    docker build -t "$IMAGE_NAME" .
  else
    echo "[*] Docker image $IMAGE_NAME už existuje (přeskakuji build)"
  fi
else
  echo "[!] Docker není dostupný, přeskakuji build"
fi

cat > "$ROOTFS_DIR/bin/docker-launch" <<'EOF'
#!/bin/bash
set -e
ROOTFS="<ROOTFS>"
IMAGE="proot-bionic:latest"
exec docker run -it --rm \
  --privileged \
  -v "$ROOTFS":/rootfs:rw \
  -v /dev:/dev \
  -v /proc:/proc \
  -v /sys:/sys \
  "$IMAGE" \
  proot -r /rootfs -0 -b /:/parrot -b /dev -b /proc -b /sys /bin/bash -l
EOF
chmod +x "$ROOTFS_DIR/bin/docker-launch"
sed -i "s#<ROOTFS>#${ROOTFS_DIR//#/\\#}#g" "$ROOTFS_DIR/bin/docker-launch"

echo ""
echo "[*] Testovací příkazy:"
echo "  Přímo na hostu:   $ROOTFS_DIR/bin/run-test"
echo "  V Dockeru:       $ROOTFS_DIR/bin/docker-launch"
echo ""
echo "[*] Hotovo:"
echo "  rootfs: $ROOTFS_DIR"
echo "  launcher: $ROOTFS_DIR/bin/proot-launch"
