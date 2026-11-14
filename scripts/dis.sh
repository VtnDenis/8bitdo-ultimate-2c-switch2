#!/bin/bash
# Disassemble one function from the current blob build. Usage: wsl -e bash scripts/dis.sh sw_pack
set -e
export PATH=$HOME/tc32/alias:$HOME/tc32/bin:$PATH
cd "$(dirname "$0")/.."
tc32-elf-objdump -d build/blob.elf | sed -n "/<$1>:/,/^\$/p"
