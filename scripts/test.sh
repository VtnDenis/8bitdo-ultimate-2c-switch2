#!/bin/bash
# Host unit tests (plain gcc, no hardware needed). Run from the repo root:
#   wsl -e bash scripts/test.sh
set -e
cd "$(dirname "$0")/.."
gcc -Wall -Wextra -I src -o /tmp/test_packer test/test_packer.c
gcc -Wall -Wextra -I src -o /tmp/test_handler test/test_handler.c
/tmp/test_packer
/tmp/test_handler
