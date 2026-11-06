#!/usr/bin/env bash
# Count kernel C/C++ line counts in a folder (no git required).
#
#   scripts/loc.sh [path]
#
# Counts *.c *.h *.cc *.hh *.cpp *.hpp in the given path (default: current
# directory). Excluded: the libc API headers (include/api/), vendored
# third-party code (external/), build output (build/), apps and tests
# (apps/, app/, tests/), and build tooling / non-source data
# (tools/, scripts/, static/).
#
# Output:
#   1. Per-folder raw line counts (mirrors the original git-grep version)
#   2. Per-language breakdown with blank/comment/code split (requires cloc)
set -euo pipefail

ROOT=${1:-.}

# Build the find exclusion prune list
EXCLUDE_DIRS=(
    external build
    apps app tests
    tools scripts static
)

# Assemble -path .../dir -prune -o ... for each excluded dir
prune_args=()
for d in "${EXCLUDE_DIRS[@]}"; do
    prune_args+=( -path "$ROOT/$d" -prune -o )
done
# Also exclude include/api subtree
prune_args+=( -path "$ROOT/include/api" -prune -o )

summarize() {
    awk -v root="$ROOT" '
    {
        lines = $1
        # $2 onward is the path; reconstruct in case of spaces
        path = $2; for (i=3; i<=NF; i++) path = path " " $i
        # strip leading ROOT/ or ROOT
        sub("^" root "/?", "", path)
        # top-level dir or "(root)"
        n = split(path, parts, "/")
        dir = (n > 1) ? parts[1] : "(root)"
        per[dir] += lines
        total += lines
    }
    END {
        for (d in per)
            printf "  %-24s %9d\n", d, per[d]
    }' | sort -k2 -nr
}

# --- 1. Per-folder summary (raw wc -l, same structure as original) ----------
echo "== Line counts by folder: $ROOT =="

find "${prune_args[@]}" \
    \( -name '*.c' -o -name '*.h' \
       -o -name '*.cc' -o -name '*.hh' \
       -o -name '*.cpp' -o -name '*.hpp' \) \
    -print0 |
    xargs -0 wc -l 2>/dev/null |
    grep -v '^ *0 ' |        # skip empty files
    grep -v ' total$' |      # skip xargs batch totals
    summarize | tee >(
        awk '{ t += $NF } END { printf "  %-24s %9d\n", "TOTAL", t }'
    )

echo ""

# --- 2. Per-language breakdown via cloc (blank / comment / code) ------------
if command -v cloc &>/dev/null; then
    echo "== Line counts by language (cloc): $ROOT =="
    CLOC_EXCLUDE=$(IFS=,; echo "${EXCLUDE_DIRS[*]}")
    cloc "$ROOT" \
        --include-ext=c,h,cc,hh,cpp,hpp \
        --exclude-dir="$CLOC_EXCLUDE" \
        --not-match-d='include/api'
else
    echo "(Install cloc for a breakdown by language / blank / comment lines:"
    echo "  apt install cloc   or   brew install cloc)"
fi
