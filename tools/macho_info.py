"""Print what is inside a Mach-O file: header, load commands, segments.

    python3 tools/macho_info.py <file> [<file> ...]

Used by scripts/build-userland.sh to see what the linker actually produced:
whether the entry point is in LC_MAIN or LC_UNIXTHREAD, whether an
LC_LOAD_DYLINKER sneaked in, and how big the segments are.
"""
import struct
import sys

MH_MAGIC = 0xfeedface
MH_MAGIC_64 = 0xfeedfacf
FAT_MAGIC = 0xcafebabe

MH_OBJECT = 0x1
MH_EXECUTE = 0x2
MH_DYLIB = 0x6
MH_DYLINKER = 0x7
MH_BUNDLE = 0x8

MH_PIE = 0x200000
MH_DYLDLINK = 0x4
MH_NOUNDEFS = 0x1

LC_SEGMENT = 0x1
LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
LC_LOAD_DYLINKER = 0xe
LC_MAIN = 0x80000028
LC_SEGMENT_SPLIT_INFO = 0x1e

CPU_TYPE_ARM64 = 0x0100000C

LC_NAMES = {
    0x1: 'LC_SEGMENT', 0x2: 'LC_SYMTAB', 0xb: 'LC_DYSYMTAB',
    0xc: 'LC_LOAD_DYLIB', 0xd: 'LC_ID_DYLIB', 0xe: 'LC_LOAD_DYLINKER',
    0x19: 'LC_SEGMENT_64', 0x1b: 'LC_UUID', 0x1d: 'LC_CODE_SIGNATURE',
    0x22: 'LC_DYLD_INFO', 0x26: 'LC_FUNCTION_STARTS', 0x29: 'LC_DATA_IN_CODE',
    0x2a: 'LC_SOURCE_VERSION', 0x32: 'LC_BUILD_VERSION',
    0x80000022: 'LC_DYLD_INFO_ONLY', 0x80000028: 'LC_MAIN',
    0x80000034: 'LC_DYLD_EXPORTS_TRIE', 0x80000033: 'LC_DYLD_CHAINED_FIXUPS',
}


def show(path):
    d = open(path, 'rb').read()
    print('=== %s (%d bytes)' % (path, len(d)))
    if len(d) < 32:
        print('  too short')
        return
    magic = struct.unpack_from('<I', d, 0)[0]
    if magic in (FAT_MAGIC, 0xbebafeca):
        print('  FAT binary, not expected here')
        return
    if magic != MH_MAGIC_64:
        print('  magic %08x (not MH_MAGIC_64)' % magic)
        return
    cputype, cpusub, filetype, ncmds, sizeofcmds, flags, _ = \
        struct.unpack_from('<iiIIIII', d, 4)
    print('  cputype %#x cpusub %#x filetype %d ncmds %d sizeofcmds %d flags %#x'
          % (cputype & 0xffffffff, cpusub & 0xffffffff, filetype, ncmds,
             sizeofcmds, flags))
    if filetype == MH_EXECUTE:
        print('  MH_EXECUTE%s%s' % (' MH_PIE' if flags & MH_PIE else '',
                                     ' MH_DYLDLINK' if flags & MH_DYLDLINK else ''))
    off = 32
    for i in range(ncmds):
        if off + 8 > len(d):
            print('  <truncated load commands>')
            break
        cmd, cmdsize = struct.unpack_from('<II', d, off)
        name = LC_NAMES.get(cmd)
        if name is None:
            name = 'LC?%#x' % cmd
        extra = ''
        if cmd == LC_SEGMENT_64:
            seg = d[off + 8:off + 24].rstrip(b'\0').decode('latin1')
            vm, vs, fo, fs = struct.unpack_from('<QQQQ', d, off + 24)
            extra = ' %-12s vm %#012x vs %#x fileoff %#x filesize %#x' % (
                seg, vm, vs, fo, fs)
        elif cmd == LC_MAIN:
            entryoff, stacksize = struct.unpack_from('<QQ', d, off + 8)
            extra = ' entryoff %#x stacksize %#x' % (entryoff, stacksize)
        elif cmd == LC_UNIXTHREAD:
            flavor, count = struct.unpack_from('<II', d, off + 8)
            extra = ' flavor %d count %d' % (flavor, count)
            if flavor == 6 and count == 68 and off + 16 + 272 <= len(d):
                pc = struct.unpack_from('<Q', d, off + 16 + 256)[0]
                extra += ' pc %#x' % pc
        elif cmd == LC_LOAD_DYLINKER:
            extra = ' %s' % d[off + 8:off + cmdsize].rstrip(b'\0').decode('latin1')
        print('  %-18s size %-5d%s' % (name, cmdsize, extra))
        off += cmdsize


if __name__ == '__main__':
    for p in sys.argv[1:]:
        show(p)
