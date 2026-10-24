#!/bin/sh
# Round-trip tests using raw image files as the "floppy" (no hardware needed).
set -e
cd "$(dirname "$0")"
APR_ABS=$(cd "$(dirname "${APRIDISK:-./apridisk}")" && pwd)/$(basename "${APRIDISK:-./apridisk}")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
A=${APRIDISK:-./apridisk}
fail() { echo "FAIL: $*"; exit 1; }

python3 - "$T" <<'PY'
import sys, os, random, struct
t = sys.argv[1]; random.seed(7)
def fill(img, secs):
    for i in range(secs):
        o = i * 512
        if i == 0: continue
        k = i % 4
        img[o:o+512] = (bytes([0xE5]) * 512 if k == 0 else bytes(512) if k == 1
                        else bytes(random.randrange(256) for _ in range(512)))
# MS-DOS 720K (BPB)
d = bytearray(1440 * 512)
d[0:3] = b'\xEB\x3C\x90'; d[3:11] = b'MSDOS5.0'
struct.pack_into('<H', d, 0x0b, 512); struct.pack_into('<H', d, 0x13, 1440)
d[0x15] = 0xF9; struct.pack_into('<H', d, 0x18, 9); struct.pack_into('<H', d, 0x1a, 2)
fill(d, 1440); open(t + '/dos720.img', 'wb').write(d)
# Apricot native, 80 track double sided
a = bytearray(80 * 2 * 9 * 512)
a[8] = 1; a[0x0b] = 3
struct.pack_into('<H', a, 0x10, 9); struct.pack_into('<H', a, 0x12, 80); a[0x16] = 2
a[0x5a] = 0xFE; a[0x5d] = 2
fill(a, 80 * 2 * 9); open(t + '/apr80ds.img', 'wb').write(a)
PY

for kind in dos720 apr80ds; do
  echo "== $kind"
  $A -d $T/$kind.img read $T/$kind.apr "Test disk $kind" > $T/$kind.log || fail "read $kind"
  grep -q "Done transferring" $T/$kind.log || fail "no completion message"
  # restore onto a blank image of the same size and compare
  head -c $(stat -c %s $T/$kind.img) /dev/zero > $T/$kind.out
  $A -d $T/$kind.out write $T/$kind.apr > $T/$kind.wlog || fail "write $kind"
  cmp $T/$kind.img $T/$kind.out || fail "$kind round trip differs"
  echo "round trip identical ($(stat -c %s $T/$kind.apr) byte image from $(stat -c %s $T/$kind.img))"
done

echo "== independent parser"
python3 - "$T" <<'PY'
import sys, struct
t = sys.argv[1]
for kind, secs, spt, heads in (('dos720', 1440, 9, 2), ('apr80ds', 1440, 9, 2)):
    img = open(f'{t}/{kind}.img', 'rb').read(); f = open(f'{t}/{kind}.apr', 'rb').read()
    assert f[:24] == b'ACT Apricot disk image\x1a\x04' and f[24:128] == bytes(104), 'header'
    pos, seen, kinds, creator, comment = 128, {}, {}, None, None
    while pos < len(f):
        typ, comp, hs, ds, ident, pad = struct.unpack_from('<IHHIIH', f, pos)
        assert hs == 0x12 and pad == 0
        data = f[pos+hs:pos+hs+ds]; pos += hs + ds
        kinds[comp] = kinds.get(comp, 0) + 1
        if comp == 0x3E5A: n, b = struct.unpack('<HB', data); data = bytes([b]) * n
        else: assert comp == 0x9E90
        if typ == 0xE31D0001:
            cyl, sec, head = ident >> 16, (ident >> 8) & 0xff, ident & 0xff
            lba = ((cyl * heads + head) * spt + sec - 1)
            assert img[lba*512:lba*512+512] == data, (cyl, head, sec)
            seen[ident] = 1
        elif typ == 0xE31D0003: creator = data
        elif typ == 0xE31D0002: comment = data
        else: raise AssertionError(hex(typ))
    assert pos == len(f) and len(seen) == secs, (len(seen), secs)
    print(kind, 'ok:', len(seen), 'sectors, compression tags', {hex(k): v for k, v in kinds.items()},
          creator, comment)
PY

echo "== truncated image (unreadable sectors are skipped and reported)"
head -c 400000 $T/dos720.img > $T/short.img
$A -d $T/short.img read $T/short.apr x > $T/short.log || fail "read short"
grep -q "sectors were unreadable" $T/short.log || fail "no warning"
grep -o '\*\*\* WARNING - [0-9]* sectors' $T/short.log

echo "== check command"
$A check $T/dos720.apr | sed -n '1,6p'
$A check $T/nonexistent.apr | grep -q "could not be opened" || fail "check missing file"

echo "== info command"
$A -d $T/dos720.img info | sed -n '1,12p'

echo "== interactive menu (piped keys): info, then quit"
printf 'iq' | $A -d $T/apr80ds.img | grep -E "Apricot boot disk|Should be able"

echo "== interactive read via menu"
( cd $T && printf 'rmenu1\n\nHello\nworld\n\n\nq' | "$APR_ABS" -d $T/dos720.img > menu.log; tail -n 4 menu.log; ls disks )
echo ALL TESTS PASSED
