#!/usr/bin/env python3
# Adapted from Aurora68 (github.com/farsonic/Aurora68) tools/mac/rsrc.py, MPL-2.0.
"""Minimal classic Mac OS resource fork and MacBinary II writer (no external dependencies).

    from rsrc import ResourceFork, macbinary
    rf = ResourceFork()
    rf.add(b'DRVR', 10, driver_bytes, name='.PiDisk', attrs=0x58)
    open('PiDisk.bin', 'wb').write(macbinary('PiDisk', b'INIT', b'PSMC', data_fork=b'', rsrc_fork=rf.build()))

MacBinary files are copied onto an HFS image with hfsutils: `hcopy -m PiDisk.bin :`.
Resource fork format: Inside Macintosh: More Macintosh Toolbox, "Resource Manager", resource file format.
"""
import struct, time

class ResourceFork:
    def __init__(self):
        self.res = []            # (type, id, name|None, attrs, data)

    def add(self, rtype, rid, data, name=None, attrs=0):
        assert len(rtype) == 4
        self.res.append((bytes(rtype), int(rid), name, attrs, bytes(data)))

    def build(self):
        # data area: 4-byte length + data per resource, in insertion order
        data_area = bytearray(); offsets = []
        for (_, _, _, _, d) in self.res:
            offsets.append(len(data_area)); data_area += struct.pack('>I', len(d)) + d
        # group by type, keep order of first appearance
        types = []
        for r in self.res:
            if r[0] not in types: types.append(r[0])
        name_list = bytearray(); ref_lists = {}; type_list = bytearray()
        # reference lists follow the type list: type list = 2-byte count-1 + 8 bytes per type
        type_list_len = 2 + 8 * len(types)
        ref_off = type_list_len
        refs_by_type = {}
        for t in types:
            items = [(i, r) for i, r in enumerate(self.res) if r[0] == t]
            refs = bytearray()
            for i, (rtype, rid, name, attrs, d) in items:
                if name is not None:
                    noff = len(name_list); nb = name.encode('mac_roman'); name_list += bytes([len(nb)]) + nb
                else:
                    noff = 0xFFFF
                refs += struct.pack('>hH', rid, noff) + bytes([attrs]) + offsets[i].to_bytes(3, 'big') + b'\0\0\0\0'
            refs_by_type[t] = (ref_off, len(items), refs)
            ref_off += len(refs)
        type_list += struct.pack('>H', (len(types) - 1) & 0xFFFF)
        for t in types:
            off, n, _ = refs_by_type[t]
            type_list += t + struct.pack('>HH', n - 1, off)
        ref_lists = b''.join(refs_by_type[t][2] for t in types)
        type_list_off = 28                      # resource map header is 28 bytes
        name_list_off = type_list_off + len(type_list) + len(ref_lists)
        # map header: 16-byte copy of the file header, next-map handle (4), file refnum (2), attributes (2),
        # type list offset (2), name list offset (2) = 28 bytes
        rmap = bytearray(16) + struct.pack('>IHHHH', 0, 0, 0, type_list_off, name_list_off) + type_list + ref_lists + name_list
        # header: data offset, map offset, data length, map length (+ 240 reserved)
        data_off = 256
        header = struct.pack('>IIII', data_off, data_off + len(data_area), len(data_area), len(rmap)) + bytes(240)
        rmap[0:16] = header[0:16]               # map begins with a copy of the header
        return bytes(header) + bytes(data_area) + bytes(rmap)


def _crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def macbinary(name, ftype, creator, data_fork=b'', rsrc_fork=b'', finder_flags=0):
    """MacBinary II image of a file (128-byte header + forks padded to 128)."""
    nb = name.encode('mac_roman')[:63]
    now = int(time.time()) + 2082844800          # seconds since 1904
    h = bytearray(128)
    h[0] = 0; h[1] = len(nb); h[2:2 + len(nb)] = nb
    h[65:69] = ftype; h[69:73] = creator
    h[73] = (finder_flags >> 8) & 0xFF
    h[83:87] = struct.pack('>I', len(data_fork)); h[87:91] = struct.pack('>I', len(rsrc_fork))
    h[91:95] = struct.pack('>I', now); h[95:99] = struct.pack('>I', now)
    h[101] = finder_flags & 0xFF
    h[122] = 129; h[123] = 129                   # MacBinary II version
    h[124:126] = struct.pack('>H', _crc16_xmodem(bytes(h[:124])))
    def pad(b): return b + bytes((-len(b)) % 128)
    return bytes(h) + pad(data_fork) + pad(rsrc_fork)


if __name__ == '__main__':
    import sys
    rf = ResourceFork(); rf.add(b'TEST', 128, b'hello', name='x')
    sys.stdout.buffer.write(macbinary('test', b'TEXT', b'ttxt', b'', rf.build()))
