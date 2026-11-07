#!/bin/sh
# Fetch the parts of the Luckfox Pico SDK we need (RV1103 uClibc toolchain,
# Rockit MPI headers/libs, rkaiq headers/libs, prebuilt librtsp) into $1.
#
# Uses a shallow, blob-filtered, sparse checkout pinned to a commit, so only
# ~350 MB is downloaded instead of the multi-GB full SDK.
#
# If LUCKFOX_SDK points at an existing luckfox-pico checkout, it is symlinked
# instead of downloading anything.
#
# usage: fetch-sdk.sh <dest-dir> <commit>     (the Makefile pins the commit)
set -eu

DEST=${1:?usage: fetch-sdk.sh <dest-dir> <commit>}
COMMIT=${2:?usage: fetch-sdk.sh <dest-dir> <commit>}
REPO=https://github.com/LuckfoxTECH/luckfox-pico.git

PATHS="
/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/
/media/rockit/rockit/mpi/sdk/include/
/media/rockit/rockit/lib/lib32/
/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/
/media/isp/release_camera_engine_rkaiq_rv1106_arm-rockchip830-linux-uclibcgnueabihf/lib/
/media/common_algorithm/common_algorithm/misc/include/
/media/common_algorithm/common_algorithm/misc/lib/arm-rockchip830-linux-uclibcgnueabihf/
"

if [ -e "$DEST" ]; then
    echo "$DEST already exists; remove it to re-fetch." >&2
    exit 0
fi

if [ -n "${LUCKFOX_SDK:-}" ]; then
    echo "Using existing SDK at $LUCKFOX_SDK"
    ln -s "$(cd "$LUCKFOX_SDK" && pwd)" "$DEST"
    exit 0
fi

echo "Fetching Luckfox Pico SDK subset @ $COMMIT into $DEST"
TMP="$DEST.partial"
rm -rf "$TMP"
git init -q "$TMP"
cd "$TMP"
git remote add origin "$REPO"
git config remote.origin.promisor true
git config remote.origin.partialclonefilter blob:none
git sparse-checkout set --no-cone $PATHS
git fetch --depth 1 --filter=blob:none origin "$COMMIT"
git checkout -q FETCH_HEAD
cd - >/dev/null
# history isn't needed (and would double the size of a CI cache)
rm -rf "$TMP/.git"
echo "$COMMIT" > "$TMP/.commit"
mv "$TMP" "$DEST"
echo "SDK ready: $(du -sh "$DEST" | cut -f1)"
