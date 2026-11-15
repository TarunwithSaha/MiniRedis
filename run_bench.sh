#!/bin/bash
# Usage: ./run_bench.sh [ops_per_client]
# Starts a FRESH server for each client count, runs the benchmark, stops the server.
OPS=${1:-20000}
SERVER=./build/server
BENCH=./build/bench

echo "Machine: $(nproc) cores | $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
echo "Memory:  $(free -h | awk '/Mem:/ {print $2}') | Kernel: $(uname -sr)"
echo "Server:  8 workers, max 100000 keys, snapshots off | Workload: 50% SET / 50% GET, $OPS ops per client"
echo

pkill -x server 2>/dev/null
sleep 0.5

for C in 1 2 4 8 16; do
  rm -f dump.snap dump.snap.tmp
  $SERVER 100000 0 > /dev/null 2>&1 &
  SERVER_PID=$!

  for i in $(seq 1 50); do nc -z localhost 6380 2>/dev/null && break; sleep 0.1; done

  echo "=== $C clients ==="
  $BENCH 127.0.0.1 6380 $C $OPS
  echo

  kill $SERVER_PID 2>/dev/null
  wait $SERVER_PID 2>/dev/null
  sleep 0.5
done
rm -f dump.snap dump.snap.tmp