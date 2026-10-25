#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
clang-19 -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined op13_bitbang.c test_bitbang.c -o test-bitbang
./test-bitbang
clang-19 -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined test_bitbang_cleanup.c -o test-bitbang-cleanup
./test-bitbang-cleanup
