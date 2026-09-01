#!/bin/bash
set -euo pipefail

ROOTFS_DIR="$(cd "$(dirname "$0")/testrootfs" && pwd)"
IMAGE_NAME="proot-bionic:latest"

if [ ! -d "$ROOTFS_DIR/bin" ]; then
  echo "[!] Rootfs neexistuje. Spusť nejprve setup-test.sh" >&2
  exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "[!] Docker není dostupný" >&2
  exit 1
fi

if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  echo "[*] Building Docker image: $IMAGE_NAME"
  if ! DOCKER_BUILDKIT=0 docker build -t "$IMAGE_NAME" .; then
    echo "[!] BuildKit build selhal, zkusím bez BuildKit..." >&2
    docker build --no-cache -t "$IMAGE_NAME" .
  fi
fi

echo "[*] Spouštím proot v Dockeru..."
docker run -it --rm \
  --privileged \
  -v "$ROOTFS_DIR":/rootfs:rw \
  -v /dev:/dev \
  -v /proc:/proc \
  -v /sys:/sys \
  "$IMAGE_NAME" \
  proot -r /rootfs -0 -b /:/parrot -b /dev -b /proc -b /sys /bin/bash -l
