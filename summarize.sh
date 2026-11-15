#!/bin/bash
# Usage: ./summarize.sh bench_run1.txt bench_run2.txt bench_run3.txt
rows=$(awk '
  /^=== [0-9]+ clients ===/ {c=$2}
  /^throughput:/ {t=$2}
  /^latency us:/ {
    for (i = 1; i <= NF; i++) {
      if ($i ~ /^avg=/) a = substr($i, 5)
      if ($i ~ /^p95=/) p95 = substr($i, 5)
      if ($i ~ /^p99=/) p99 = substr($i, 5)
    }
    print c, t, a, p95, p99
  }' "$@")

median() { sort -n | awk '{v[NR]=$1} END {print v[int((NR+1)/2)]}'; }

echo "Median of $# runs"
printf "%-8s %-16s %-10s %-10s %-10s\n" Clients "Throughput(ops/s)" "Avg(us)" "p95(us)" "p99(us)"
for C in 1 2 4 8 16; do
  thr=$(echo "$rows" | awk -v c=$C '$1==c {print $2}' | median)
  avg=$(echo "$rows" | awk -v c=$C '$1==c {print $3}' | median)
  p95=$(echo "$rows" | awk -v c=$C '$1==c {print $4}' | median)
  p99=$(echo "$rows" | awk -v c=$C '$1==c {print $5}' | median)
  printf "%-8s %-16s %-10s %-10s %-10s\n" $C $thr $avg $p95 $p99
done