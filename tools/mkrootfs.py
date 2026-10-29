"""Build the read-only image that mockfs serves as the root filesystem.

Layout (little endian, see bsd/miscfs/mockfs/mockfs_vfsops.c):

    0x00  u32 magic 'P','O','L','F'
    0x04  u32 version
    0x08  u32 nfiles
    0x0c  u32 fileid0        (unused, 0)
    0x10  u64 data_off       (page aligned)
    0x18  u64 image_size
    0x20  records, each followed by the name bytes (no NUL):
            u16 namelen, u16 type, u32 mode, u32 uid, u32 gid, u32 fileid,
            u64 size, u64 off, u64 mtime

Every file's data starts on a 512 byte boundary (the device block size, so
mockfs_blockmap() can turn a file offset into a device block with a plain
division).

    python tools/mkrootfs.py work/rootfs work/polaris_rootfs.img
    python tools/mkrootfs.py work/rootfs work/polaris_rootfs.img --emit-c

The image is padded to exactly IMAGE_SIZE bytes because it is embedded in the
kernel as a fixed size array; --emit-c prints that array.
"""
import argparse
import os
import struct
import sys

MAGIC = 0x464C4F50          # 'P','O','L','F'
VERSION = 1
CONSOLE = 4                # a file whose writes go to the kernel console
# 20 KB: the table plus /etc/hostname, /etc/motd and a 16 KB /sbin/launchd.
# The launchd file has to be a full kernel page (16 KB), see tools/mkstub2.py.
IMAGE_SIZE = 20480
DIR, REG, LNK = 1, 2, 3
MTIME = 1600000000
CONSOLE_PATH = '/dev/console'


def align(n, a):
    return (n + a - 1) // a * a


def collect(src):
    """Return [(path, type, data)] with parents before children."""
    entries = [('/', DIR, b'')]

    for root, dirs, files in os.walk(src):
        dirs.sort()
        files.sort()
        rel = os.path.relpath(root, src)
        base = '/' if rel == '.' else '/' + rel.replace('\\', '/')
        if base != '/':
            entries.append((base, DIR, b''))
        for name in files:
            p = os.path.join(root, name)
            with open(p, 'rb') as f:
                entries.append((base.rstrip('/') + '/' + name, REG, f.read()))

    # Parents sort before their children because a prefix compares shorter.
    entries.sort(key=lambda e: (e[0].count('/'), e[0]))
    return entries


def build(src, image_size=None):
    entries = collect(src)

    recs = []
    table_len = 32
    for i, (path, typ, data) in enumerate(entries):
        name = path.encode()
        assert len(name) <= 255, path
        # The kernel console is a file with no content: writing to it prints
        # on the screen.  It is the only channel user space has for now.
        if path == CONSOLE_PATH:
            typ, data = CONSOLE, b''
        recs.append((name, typ, data, i + 1))
        table_len += 44 + len(name)

    data_off = align(table_len, 512)

    cursor = data_off
    placed = []
    for name, typ, data, fileid in recs:
        if typ == REG and data:
            cursor = align(cursor, 512)
            placed.append((name, typ, data, fileid, cursor))
            cursor += len(data)
        else:
            placed.append((name, typ, data, fileid, 0))

    limit = image_size if image_size else IMAGE_SIZE
    image_size = align(cursor, limit) if cursor else limit
    assert image_size >= limit, (
        'image needs %d bytes, the kernel array holds %d' % (image_size, limit))

    buf = bytearray(image_size)
    struct.pack_into('<IIIIQQ', buf, 0, MAGIC, VERSION, len(placed), 0,
        data_off, cursor)

    off = 32
    for name, typ, data, fileid, foff in placed:
        size = len(data) if typ == REG else 0
        # The console is written by user space, everybody may write to it.
        mode = 0o622 if typ == CONSOLE else 0o755
        struct.pack_into('<HHIIIIQQQ', buf, off, len(name), typ, mode,
            0, 0, fileid, size, foff, MTIME)
        off += 44
        buf[off:off + len(name)] = name
        off += len(name)
        if foff:
            buf[foff:foff + len(data)] = data

    assert off <= data_off

    return bytes(buf), placed, cursor


def carray(d):
    """C initialiser for the image, without the trailing NULs (C pads with 0)."""
    end = len(d)
    while end > 0 and d[end - 1] == 0:
        end -= 1
    out = []
    for i in range(0, end, 12):
        out.append('\t' + ' '.join('0x%02x,' % b for b in d[i:i + 12]))
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--emit-c', action='store_true')
    args = ap.parse_args()

    img, placed, used = build(args.src)
    with open(args.out, 'wb') as f:
        f.write(img)

    sys.stderr.write('wrote %s (%d bytes, %d bytes used, %d entries)\n'
        % (args.out, len(img), used, len(placed)))
    for name, typ, data, fileid, foff in placed:
        sys.stderr.write('  %-20s type=%d id=%-3d off=0x%05x size=%d\n'
            % (name.decode(), typ, fileid, foff, len(data)))

    if args.emit_c:
        sys.stdout.write(carray(img))


if __name__ == '__main__':
    main()
