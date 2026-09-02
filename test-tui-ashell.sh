#!/bin/bash
D=/data/user/0/com.linux_core/files
R=$D/nh/distro/parrot
L=$D/usr/bin/elf_loader
RESULTS=/root/elf_loader/results/tui_$(date +%Y%m%d_%H%M%S).txt
> "$RESULTS"
PASS=0
FAIL=0
for b in top htop btop btm; do
  p=$(find "$R" -name "$b" -type f 2>/dev/null | head -1)
  if [ -z "$p" ]; then
    echo "SKIP $b: not found" >> "$RESULTS"
    continue
  fi
  echo "=== $b ===" >> "$RESULTS"
  tmux kill-session -t tuitest 2>/dev/null || true
  tmux new-session -d -s tuitest "ashell -c '$L --ownall $p'"
  sleep 3
  if tmux has-session -t tuitest 2>/dev/null; then
    tmux send-keys -t tuitest 'q' 2>/dev/null || true
    sleep 1
    tmux send-keys -t tuitest C-c 2>/dev/null || true
    sleep 1
    tmux kill-session -t tuitest 2>/dev/null || true
    echo "PASS $b" >> "$RESULTS"
    PASS=$((PASS+1))
  else
    echo "FAIL $b: tmux session died" >> "$RESULTS"
    FAIL=$((FAIL+1))
  fi
done
echo "PASS=$PASS FAIL=$FAIL" >> "$RESULTS"
cat "$RESULTS"
