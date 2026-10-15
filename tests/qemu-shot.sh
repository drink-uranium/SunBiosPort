#!/bin/sh
# Usage: tests/qemu-shot.sh <out.png> <seconds> [keys-file] -- <qemu args...>
# Boots QEMU headless, optionally types the lines in keys-file at the ok
# prompt, then saves a screenshot and the serial log next to it.
out=$1; secs=$2; keys=$3; shift 3
[ "$1" = "--" ] && shift
sock=$(mktemp -u /tmp/qmon.XXXX)
qemu-system-x86_64 -display none -monitor unix:$sock,server,nowait \
	-serial file:${out%.png}.serial.log "$@" &
pid=$!
sleep "$secs"
if [ -n "$keys" ] && [ -f "$keys" ]; then
	python3 - "$sock" "$keys" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
names = {' ': 'spc', '-': 'minus', '=': 'equal', '/': 'slash', '.': 'dot', ',': 'comma',
         ';': 'semicolon', "'": 'apostrophe', '\\': 'backslash', '[': 'bracket_left',
         ']': 'bracket_right', '`': 'grave_accent', '\n': 'ret', '*': 'shift-8', '"': 'shift-apostrophe',
         ':': 'shift-semicolon', '?': 'shift-slash', '@': 'shift-2', '#': 'shift-3', '(': 'shift-9',
         ')': 'shift-0', '!': 'shift-1', '+': 'shift-equal', '<': 'shift-comma', '>': 'shift-dot',
         '_': 'shift-minus', '&': 'shift-7', '$': 'shift-4', '%': 'shift-5', '^': 'shift-6', '|': 'shift-backslash', '~': 'shift-grave_accent'}
for line in open(sys.argv[2]):
    if line.startswith('#sleep'):
        time.sleep(float(line.split()[1])); continue
    for ch in line:
        k = names.get(ch, ch)
        if ch.isupper(): k = 'shift-' + ch.lower()
        s.send(('sendkey %s\n' % k).encode()); time.sleep(0.04)
    time.sleep(0.6)
time.sleep(1.5)
PY
fi
printf 'screendump %s\n' "${out%.png}.ppm" | python3 -c "
import socket,sys,time
s=socket.socket(socket.AF_UNIX); s.connect('$sock'); time.sleep(0.3)
s.send(sys.stdin.read().encode()); time.sleep(1.5); s.send(b'quit\n'); time.sleep(0.5)"
wait $pid 2>/dev/null
python3 -c "from PIL import Image; Image.open('${out%.png}.ppm').save('$out')" && rm -f "${out%.png}.ppm"
