#!/bin/bash
# Usage: ./load_test.sh [clients] [commands_per_client]
CLIENTS=${1:-8}
N=${2:-5000}

# Start all clients at the same time. Each sends N SETs with unique keys.
for c in $(seq 1 $CLIENTS); do
  seq 1 $N | sed "s/.*/SET c${c}_k& x/" | nc -q1 localhost 6380 > /dev/null 2>&1 &
done
wait

echo "Expected keys: $((CLIENTS * N))"
echo "Server says:   $(printf 'DBSIZE\n' | nc -q1 localhost 6380)"