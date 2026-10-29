"""Build the root filesystem image from the compiled userland and patch it in.

The kernel carries the root filesystem as a C array in bsd/kern/bsd_init.c.
That array has to hold the *compiled* userland (our launchd and hello), and
the userland is compiled on the runner, so the array cannot live in the patch
series: this script builds the image and writes the patch that puts it into
the tree.

    python3 tools/mkuserland.py --xnu <xnu checkout> --binaries <dir> \\
        --patch <file>

<dir> holds launchd.bin and hello.bin.  The script replaces the
polaris_root_image array in bsd/kern/bsd_init.c and writes the resulting git
diff to <file>.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkrootfs

# The kernel's page size on this board (arm64 with more than 1 GB of RAM):
# a Mach-O has to be a whole number of pages, because the kernel reads
# round_page(sizeof(mach_header) + sizeofcmds) bytes of load commands in one
# go and answers LOAD_BADMACHO when the file is shorter than that.
PAGE = 16384


def pad(data):
    if len(data) % PAGE:
        data += b'\0' * (PAGE - (len(data) % PAGE))
    return data


def build_rootfs(binaries):
    """Lay out the root filesystem: the text files plus the two programs."""
    d = tempfile.mkdtemp(prefix='polaris-rootfs-')
    os.makedirs(os.path.join(d, 'etc'))
    os.makedirs(os.path.join(d, 'dev'))
    os.makedirs(os.path.join(d, 'sbin'))

    with open(os.path.join(d, 'etc', 'motd'), 'wb') as f:
        f.write(b'HELLO POLARIS\n')
    with open(os.path.join(d, 'etc', 'hostname'), 'wb') as f:
        f.write(b'polaris\n')
    # An empty file here becomes the console: writes to it are printed by the
    # kernel (see bsd/miscfs/mockfs/mockfs_vnops.c).
    open(os.path.join(d, 'dev', 'console'), 'wb').close()

    for name, target in (('launchd.bin', 'launchd'), ('hello.bin', 'hello')):
        src = os.path.join(binaries, name)
        with open(src, 'rb') as f:
            data = f.read()
        with open(os.path.join(d, 'sbin', target), 'wb') as f:
            f.write(pad(data))

    return d


def carray(d):
    """C initialiser for the image, without the trailing NULs (C pads with 0)."""
    end = len(d)
    while end > 0 and d[end - 1] == 0:
        end -= 1
    out = []
    for i in range(0, end, 12):
        out.append('\t' + ' '.join('0x%02x,' % b for b in d[i:i + 12]))
    return '\n'.join(out) + '\n'


def patch_binit(path, img):
    s = open(path, 'rb').read().decode('utf-8')
    lines = s.split('\n')
    start = next(i for i, l in enumerate(lines)
                 if re.search(r'polaris_root_image\[\d+\]', l))
    end = next(i for i in range(start, len(lines)) if lines[i] == '};')

    new = ['const unsigned char polaris_root_image[%d]' % len(img),
           '    __attribute__((aligned(4096), used)) = {']
    new += carray(img).rstrip('\n').split('\n')
    new += ['};']
    lines[start:end + 1] = new
    open(path, 'wb').write('\n'.join(lines).encode('utf-8'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--xnu', required=True)
    ap.add_argument('--binaries', required=True)
    ap.add_argument('--patch', required=True)
    args = ap.parse_args()

    root = build_rootfs(args.binaries)
    img, placed, used = mkrootfs.build(root)
    sys.stderr.write('rootfs image: %d bytes (%d used, %d entries)\n'
                     % (len(img), used, len(placed)))
    for name, typ, data, fileid, foff in placed:
        sys.stderr.write('  %-20s type=%d id=%-3d off=0x%05x size=%d\n'
                         % (name.decode(), typ, fileid, foff, len(data)))

    binit = os.path.join(args.xnu, 'bsd', 'kern', 'bsd_init.c')
    patch_binit(binit, img)

    diff = subprocess.run(['git', '-C', args.xnu, 'diff', '--',
                           'bsd/kern/bsd_init.c'],
                          capture_output=True, check=True).stdout
    with open(args.patch, 'wb') as f:
        f.write(diff)
    sys.stderr.write('wrote %s (%d bytes)\n' % (args.patch, len(diff)))

    # Put the tree back the way the patch series left it: the caller applies
    # the patch, so the file must not carry the change twice.
    subprocess.run(['git', '-C', args.xnu, 'checkout', '--',
                    'bsd/kern/bsd_init.c'], check=True)


if __name__ == '__main__':
    main()
