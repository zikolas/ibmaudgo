#!/bin/sh
# assemble IBMAUDGO.COM (the same under DOS: nasm -f bin IBMAUDGO.ASM -o IBMAUDGO.COM)
set -e
cd "$(dirname "$0")"
nasm -f bin IBMAUDGO.ASM -o IBMAUDGO.COM
ls -l IBMAUDGO.COM
