#!/bin/sh
# Nightly branch: copies the build into the port 1.5 test folder, never the
# stable install next to this source folder. Set GAME to deploy elsewhere.
set -e
cd "$(dirname "$0")"
GAME="${GAME:-../../DBZ-Budokai-3-HD-Collection-v1.5-test}"
cp data/text.tsv "$GAME/Budokai3Access/text.tsv"
cp out/Budokai3Access/Budokai3Access.dll "$GAME/Budokai3Access/Budokai3Access.dll"
# The loader wraps the port's graphics plugin. The launcher puts it in place
# (and does so again after any port update), so the build uses it too.
mkdir -p "$GAME/Budokai3Access/loader"
cp out/rexgpu-access.dll "$GAME/Budokai3Access/loader/rexgpu-xenos.dll"
cp "out/Play Budokai 3 Access.exe" "$GAME/Play Budokai 3 Access.exe"
"$GAME/Play Budokai 3 Access.exe" --repair || echo "launcher repair failed; see $GAME/Budokai3Access/launch.log"
