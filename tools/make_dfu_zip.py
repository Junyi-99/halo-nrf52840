#!/usr/bin/env python3
"""Build a Nordic DFU package (zip) for the nRF52840 dongle's open bootloader.

Equivalent to `nrfutil pkg generate --hw-version 52 --sd-req 0x00
--application app.hex --application-version 1 out.zip` from pc-nrfutil, for
machines where that tool is unavailable (e.g. aarch64 Linux). No signature:
the dongle's open bootloader does not check one.
"""
import argparse
import hashlib
import json
import zipfile

from intelhex import IntelHex


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def field_varint(num, value):
    return varint(num << 3 | 0) + varint(value)


def field_bytes(num, data):
    return varint(num << 3 | 2) + varint(len(data)) + data


def init_packet(app, hw_version, fw_version, sd_req):
    # dfu-cc.proto: Packet.command (1) -> Command{op_code=INIT(1), init(2)}
    sha = hashlib.sha256(app).digest()[::-1]
    hash_msg = field_varint(1, 3) + field_bytes(2, sha)             # SHA256
    sd_req_packed = b''.join(varint(v) for v in sd_req)
    init = (field_varint(1, fw_version) +
            field_varint(2, hw_version) +
            field_bytes(3, sd_req_packed) +                            # packed repeated
            field_varint(4, 0) +                                       # APPLICATION
            field_varint(5, 0) + field_varint(6, 0) +                  # sd_size, bl_size
            field_varint(7, len(app)) +
            field_bytes(8, hash_msg) +
            field_varint(9, 0))                                        # is_debug false
    command = field_varint(1, 1) + field_bytes(2, init)
    return field_bytes(1, command)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('hexfile')
    ap.add_argument('out_zip')
    ap.add_argument('--hw-version', type=int, default=52)
    ap.add_argument('--application-version', type=int, default=1)
    ap.add_argument('--sd-req', default='0x00', help='comma separated SoftDevice IDs')
    a = ap.parse_args()

    ih = IntelHex(a.hexfile)
    app = ih.tobinstr()
    sd_req = [int(x, 0) for x in a.sd_req.split(',')]
    dat = init_packet(app, a.hw_version, a.application_version, sd_req)
    manifest = {'manifest': {'application': {'bin_file': 'app.bin', 'dat_file': 'app.dat'}}}
    with zipfile.ZipFile(a.out_zip, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('manifest.json', json.dumps(manifest, indent=2))
        z.writestr('app.bin', app)
        z.writestr('app.dat', dat)
    print(f'{a.out_zip}: app {len(app)} bytes from 0x{ih.minaddr():X}, init packet {len(dat)} bytes')


if __name__ == '__main__':
    main()
