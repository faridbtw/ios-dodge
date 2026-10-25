#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
clang-19 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. test_preflight.c fifo_snapshot.c -o test-geni
./test-geni
bash build-probe.sh
