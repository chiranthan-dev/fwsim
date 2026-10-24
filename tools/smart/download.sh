#!/bin/sh
# download.sh - fetch the Backblaze Drive Stats quarters used by the model.
#
#   tools/smart/download.sh [DATA_DIR]      (default: tools/smart/data)
#
# Backblaze publishes one zip per quarter (one CSV per day, one row per
# drive) at https://www.backblaze.com/cloud-storage/resources/hard-drive-test-data
# The zips are ~1 GB each and are NOT committed (see .gitignore). We never
# unpack them: ingest.py streams one daily CSV at a time out of the zip.
set -eu
DATA_DIR=${1:-tools/smart/data}
QUARTERS="Q1_2026 Q2_2026"
BASE=https://f001.backblazeb2.com/file/Backblaze-Hard-Drive-Data
mkdir -p "$DATA_DIR"
for q in $QUARTERS; do
    f="$DATA_DIR/data_$q.zip"
    if [ -s "$f" ] && python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).namelist()" "$f" 2>/dev/null; then
        echo "have $f"
        continue
    fi
    echo "downloading $BASE/data_$q.zip -> $f"
    # Large transfers from B2 sometimes drop mid-way: resume (-C -) until done.
    n=0
    until curl -fL --retry 3 -C - -o "$f" "$BASE/data_$q.zip"; do
        n=$((n + 1))
        [ $n -lt 20 ] || { echo "giving up on $q" >&2; exit 1; }
        echo "transfer interrupted, resuming ($n)" >&2
        sleep 5
    done
done
ls -l "$DATA_DIR"/data_*.zip
