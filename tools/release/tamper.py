#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes a tampered copy of a signed AirWriter image, for testing refusals (W-1048).

    python tools/release/tamper.py airwriter-esp32s3.bin tampered.bin

Changes one string in the image the way an attacker would: the image's XOR
checksum and appended SHA-256 still check out, so only the signature, which
covers the original bytes, can tell. A board must refuse it, over /update and
from GitHub. Run it in the ESP-IDF Python environment (esptool).
"""
import hashlib, sys
from esptool.bin_image import LoadFirmwareImage

OLD = b"ImageWriter II"     # the default queue name, in the app's rodata

def main(src, dst):
    data = bytearray(open(src, "rb").read())
    img = LoadFirmwareImage("esp32s3", src)
    end = img.data_length                   # the checksummed image; its SHA-256 follows
    assert img.append_digest and img.stored_digest == img.calc_digest, "not a valid image"
    at = data.find(OLD)
    assert 0 < at < end, "the string is not in the image"
    # The same bit in two bytes: the XOR checksum does not move.
    data[at] ^= 0x01                        # "I" -> "H"
    data[at + 6] ^= 0x01                    # "r" -> "s"
    data[end:end + 32] = hashlib.sha256(data[:end]).digest()
    open(dst, "wb").write(data)

    out = LoadFirmwareImage("esp32s3", dst)
    assert out.stored_digest == out.calc_digest, "the rewritten hash does not check"
    print("%s: %r -> %r at 0x%x; checksum and hash valid, signature now wrong"
          % (dst, OLD.decode(), bytes(data[at:at + len(OLD)]).decode(), at))

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
