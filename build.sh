#!/bin/sh
# Builds Budokai3Access with the 64-bit w64devkit. Output goes to out/:
#   out/rexgpu-access.dll            the loader, a GPU plugin (goes next to dbz3.exe)
#   out/Budokai3Access/Budokai3Access.dll    the mod
set -e
cd "$(dirname "$0")"
# Set W64DEVKIT if your w64devkit is somewhere else.
W64DEVKIT="${W64DEVKIT:-$HOME/tools/w64x64/w64devkit}"
export PATH="$W64DEVKIT/bin:$PATH"
MH=third_party/minhook
mkdir -p out/Budokai3Access
windres src/plugin.rc -O coff -o out/plugin_res.o
windres src/core.rc -O coff -o out/core_res.o
windres src/launch.rc -O coff -o out/launch_res.o
gcc -O2 -Wall -s -mwindows -municode -o "out/Play Budokai 3 Access.exe" src/launch.c out/launch_res.o -lversion -static-libgcc
gcc -O2 -Wall -shared -s -o out/rexgpu-access.dll src/plugin.c src/plugin.def out/plugin_res.o -static-libgcc
gcc -O2 -Wall -Wno-unused-function -shared -o out/Budokai3Access/Budokai3Access.dll \
  src/dllmain.c src/speech.c src/buttons.c src/guest.c src/game.c src/pad.c src/text.c src/tasks.c src/menus.c src/fight.c src/du.c src/imgui.c src/roster.c src/health.c \
  $MH/src/hook.c $MH/src/buffer.c $MH/src/trampoline.c $MH/src/hde/hde64.c \
  out/core_res.o -I$MH/include -Ithird_party/prism -Isrc -static-libgcc -lole32 -luuid -lsapi -lwinmm -lversion
echo built out/rexgpu-access.dll out/Budokai3Access/Budokai3Access.dll
