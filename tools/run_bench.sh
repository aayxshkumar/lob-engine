#!/usr/bin/env bash
# Build Release, run the benchmarks, and keep the raw output next to the code it measured.
#
#     tools/run_bench.sh [recording.lobrec]
#
# Writes bench/results/<date>-<machine>-<sha7>/:
#   bench.json   Google Benchmark JSON, 10 repetitions, interleaved, aggregates and every run
#   replay.txt   lob_replay output, if a recording is given (recordings aren't committed)
#   meta.txt     commit, machine, OS, compiler and load average (run it on an idle machine)
#
# It refuses to run if the code differs from the commit, so a result always names
# exactly what produced it.
set -euo pipefail
cd "$(dirname "$0")/.."

if ! git diff --quiet HEAD -- include src bench CMakeLists.txt; then
    echo "run_bench: uncommitted changes under include/ src/ bench/ or CMakeLists.txt; commit them first" >&2
    exit 1
fi

sha=$(git rev-parse --short=7 HEAD)
machine=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || grep -m1 'model name' /proc/cpuinfo | cut -d: -f2)
slug=$(echo "$machine" | tr '[:upper:]' '[:lower:]' | sed -E 's/apple //; s/[^a-z0-9]+/-/g; s/^-|-$//g')
out="bench/results/$(date +%F)-${slug}-${sha}"
mkdir -p "$out"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLOB_BUILD_TOOLS=ON >/dev/null
cmake --build build -j >/dev/null

{
    echo "commit   $(git rev-parse HEAD)"
    echo "machine  $machine"
    echo "os       $(uname -srm)"
    echo "compiler $(c++ --version | head -1)"
    echo "flags    Release (-O3), see CMakeLists.txt"
    echo "date     $(date -u +%FT%TZ)"
    echo "load     $(sysctl -n vm.loadavg 2>/dev/null || cat /proc/loadavg)"
} > "$out/meta.txt"

./build/lob_bench --benchmark_repetitions=10 --benchmark_enable_random_interleaving=true --benchmark_out="$out/bench.json" --benchmark_out_format=json

if [[ $# -ge 1 ]]; then
    ./build/lob_replay "$1" | tee "$out/replay.txt"
fi

echo "results in $out"
