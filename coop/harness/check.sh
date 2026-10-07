#!/bin/sh
# Build, regenerate the offsets, and run every co-op check.
#
# One command because the regeneration step is the one that gets forgotten, and
# forgetting it does not fail -- it produces a suite full of detailed,
# plausible, entirely fictional failures. rig.mjs refuses to run on stale
# offsets now, but not having to think about it is better than being told off.
set -e
cd "$(dirname "$0")/../.."

make modern -j"$(nproc)"
python3 tools/coop/emit_offsets.py coop/harness/coop-offsets.json
python3 tools/coop/emit_config.py pokeemerald.map coop/wrapper/public/coop-config.json
cp coop/wrapper/public/coop-config.json coop/wrapper/dist/coop-config.json

cd coop/harness
for stage in stage1 stage2 stage3; do
    echo
    echo "=== $stage ==="
    node "$stage.mjs" ../../pokeemerald.gba
done
