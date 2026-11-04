#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p validation
exec > >(tee validation/test.log) 2>&1
flags=(-Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I.)
for t in tests/test_power.c tests/test_avdd.c test_first_read.c test_native_preflight.c; do
 clang-19 "${flags[@]}" "$t" -o validation/test
 validation/test
done
clang-19 "${flags[@]}" panel-prep/sm8750_panel.c panel-prep/test_sequence.c -o validation/panel
validation/panel
bash build-probe.sh
