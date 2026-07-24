#!/bin/bash
# Reproducible Ghidra headless RE of the rknand FTL/NFC.
# Prereqs: Ghidra 12 (brew), JDK 21. Provide the extracted raw kernel Image + System.map.
set -e
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}
export PATH="$JAVA_HOME/bin:$PATH"
HL=${GHIDRA_HEADLESS:-/opt/homebrew/Cellar/ghidra/12.1.2/libexec/support/analyzeHeadless}
HERE=$(cd "$(dirname "$0")" && pwd)
K=${1:?usage: run.sh <kernel-4.4 dir with Image + System.map + targets.txt>}
PROJ=$(mktemp -d)
"$HL" "$PROJ" rknand \
  -import "$K/vmlinux-4.4.194.bin" \
  -loader BinaryLoader -loader-baseAddr 0xb0008000 \
  -processor "ARM:LE:32:v7" -noanalysis \
  -scriptPath "$HERE" \
  -postScript RknandRE.java "$K/System.map-4.4.194-rk322x" "$(cat "$K/targets.txt")" "$K/decompiled" \
  -deleteProject
rm -rf "$PROJ"
