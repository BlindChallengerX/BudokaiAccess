#!/bin/sh
# Copies the build into the game folder (next to this source folder).
set -e
cd "$(dirname "$0")"
cp data/text.tsv ../Budokai3Access/text.tsv
cp out/Budokai3Access/Budokai3Access.dll ../Budokai3Access/Budokai3Access.dll
# The loader wraps the port's graphics plugin. The launcher puts it in place
# (and does so again after any port update), so the build uses it too.
mkdir -p ../Budokai3Access/loader
cp out/rexgpu-access.dll ../Budokai3Access/loader/rexgpu-xenos.dll
cp "out/Play Budokai 3 Access.exe" "../Play Budokai 3 Access.exe"
"../Play Budokai 3 Access.exe" --repair || echo "launcher repair failed; see Budokai3Access/launch.log"
