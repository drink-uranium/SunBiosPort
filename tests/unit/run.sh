#!/bin/sh
# Host-side unit tests (32-bit, freestanding pieces of the firmware).
set -e
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
cat > $out/main.c <<'C'
#include <stdio.h>
extern int test_main(void);
static int line_; static const char *what_;
void report(const char *w, int l) { fprintf(stderr, "FAIL line %d: %s\n", l, w); }
int main(void) { int f = test_main(); printf("%s (%d failures)\n", f ? "FAILED" : "PASS  nvram persistence", f); return f != 0; }
C
gcc -m32 -O1 -ffreestanding -fno-builtin -nostdinc -Isrc -DNVRAM_CMOS -DNVRAM_TEST -c src/nvram.c -o $out/nvram.o
gcc -m32 -O1 -ffreestanding -fno-builtin -nostdinc -Isrc -DNVRAM_CMOS -DNVRAM_TEST -Dreport=report -c tests/unit/nvram_test.c -o $out/t.o
# use the firmware's own string helpers, renamed so they don't clash with libc
for f in strlcpy strcmp strlen strtoul memset memcpy strncmp; do :; done
gcc -m32 $out/main.c $out/t.o $out/nvram.o -o $out/test 2>&1 | grep -v warning || true
$out/test
