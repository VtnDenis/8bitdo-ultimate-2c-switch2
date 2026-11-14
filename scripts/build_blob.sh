#!/bin/bash
# Build the injected TC32 blob + hook bytes. Run from the repo root:
#   wsl -e bash scripts/build_blob.sh
# Needs tc32-elf-gcc 4.5.1 from https://github.com/flyskywhy/tc32 in ~/tc32/bin,
# plus ~/tc32/alias/{as,ld} symlinks: the driver shells out to bare `as` / `ld`.
set -e
export PATH=$HOME/tc32/alias:$HOME/tc32/bin:$PATH
cd "$(dirname "$0")/.."
mkdir -p build

CFLAGS="-Os -Wall -Werror -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -fno-stack-protector"
for f in src/packer.c src/handler.c src/glue.c; do
  tc32-elf-gcc $CFLAGS -c "$f" -o "/tmp/$(basename "${f%.c}").o"
done
tc32-elf-ld -T src/blob.ld --no-check-sections -o build/blob.elf /tmp/packer.o /tmp/handler.o /tmp/glue.o
tc32-elf-objcopy -O binary -j .text build/blob.elf build/blob.bin
for h in aa68 aa2e a45c; do
  tc32-elf-objcopy -O binary -j ".hook$h" build/blob.elf "build/hook_$h.bin"
done
echo "--- symbols (last bss symbol must end at or below 0x8433B8)"
tc32-elf-nm -n build/blob.elf
echo "--- sizes"
ls -l build/blob.bin build/hook_*.bin
echo "--- hook bytes"
od -An -tx1 build/hook_aa68.bin build/hook_aa2e.bin build/hook_a45c.bin
