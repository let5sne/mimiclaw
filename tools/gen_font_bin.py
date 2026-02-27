#!/usr/bin/env python3
"""
Generate MCFN binary font file from GNU Unifont .hex data.

MCFN format:
  [0..3]   magic "MCFN" (LE: 0x4E46434D)
  [4..7]   uint32 LE glyph_count
  [8..]    glyph_count x uint32 LE codepoints (sorted)
  [..]     glyph_count x 32-byte bitmaps (same order)

Usage:
  python tools/gen_font_bin.py unifont-16.0.02.hex spiffs_data/fonts/unifont_cjk.bin

Download Unifont hex from:
  https://unifoundry.com/pub/unifont/unifont-16.0.02/font-builds/unifont-16.0.02.hex.gz
"""

import struct
import sys
import os
import gzip

# Common CJK codepoint ranges to include
RANGES = [
    # CJK punctuation & symbols
    (0x3000, 0x303F),
    # Hiragana (useful for Japanese mixed text)
    (0x3040, 0x309F),
    # Katakana
    (0x30A0, 0x30FF),
    # CJK Unified Ideographs (common subset: ~3500 most frequent)
    (0x4E00, 0x9FFF),
    # Fullwidth forms (，。！？etc)
    (0xFF00, 0xFF5E),
    # CJK Compatibility Ideographs (small set)
    (0xF900, 0xFAFF),
    # Bopomofo
    (0x3100, 0x312F),
    # CJK radicals supplement
    (0x2E80, 0x2EFF),
]

# Top ~3500 most frequent Chinese characters (GB2312 level 1).
# If the hex file has them, they'll be included. We use ranges above
# which cover all of CJK Unified Ideographs; the file size is controlled
# by what's actually present in the hex file.

# Optional: restrict to only the most common ~3500 chars to save space.
# This list covers GB2312 Level 1 (3755 chars): U+4E00..U+9FA5
# Set to True to include ALL CJK in the hex file (larger), False for common only.
INCLUDE_ALL_CJK = False

# If restricting, use frequency-based top chars. We'll use a simpler approach:
# include GB2312 Level 1 range which covers the most common characters.
# GB2312 Level 1 is roughly the first 3755 most common simplified Chinese chars.
# They're scattered across the CJK block, so we load a frequency list if available,
# otherwise include all CJK present in the hex file up to a limit.
MAX_CJK_GLYPHS = 4000  # Cap to control file size


def in_range(cp):
    """Check if codepoint is in our desired ranges."""
    for lo, hi in RANGES:
        if lo <= cp <= hi:
            return True
    return False


def parse_hex_file(path):
    """Parse GNU Unifont .hex file. Returns dict of {codepoint: bytes(32)}."""
    glyphs = {}

    opener = gzip.open if path.endswith('.gz') else open
    with opener(path, 'rt', encoding='ascii', errors='ignore') as f:
        for line in f:
            line = line.strip()
            if not line or ':' not in line:
                continue
            cp_str, hex_data = line.split(':', 1)
            try:
                cp = int(cp_str, 16)
            except ValueError:
                continue

            if not in_range(cp):
                continue

            raw = bytes.fromhex(hex_data)

            if len(raw) == 16:
                # 8x16 glyph -> expand to 16x16 by doubling width
                # Each row: 1 byte -> 2 bytes (original in high byte, low byte = 0)
                expanded = bytearray(32)
                for row in range(16):
                    expanded[row * 2] = raw[row]
                    expanded[row * 2 + 1] = 0
                glyphs[cp] = bytes(expanded)
            elif len(raw) == 32:
                # Already 16x16
                glyphs[cp] = raw
            # Skip other sizes

    return glyphs


def write_mcfn(glyphs, output_path):
    """Write MCFN binary font file."""
    # Sort by codepoint
    sorted_cps = sorted(glyphs.keys())

    # Apply CJK limit if not including all
    if not INCLUDE_ALL_CJK:
        # Separate CJK ideographs from punctuation/symbols
        cjk_chars = [cp for cp in sorted_cps if 0x4E00 <= cp <= 0x9FFF]
        non_cjk = [cp for cp in sorted_cps if not (0x4E00 <= cp <= 0x9FFF)]

        if len(cjk_chars) > MAX_CJK_GLYPHS:
            cjk_chars = cjk_chars[:MAX_CJK_GLYPHS]

        sorted_cps = sorted(non_cjk + cjk_chars)

    count = len(sorted_cps)

    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)

    with open(output_path, 'wb') as f:
        # Header
        f.write(struct.pack('<I', 0x4E46434D))  # "MCFN"
        f.write(struct.pack('<I', count))

        # Index table (sorted codepoints)
        for cp in sorted_cps:
            f.write(struct.pack('<I', cp))

        # Bitmap data
        for cp in sorted_cps:
            f.write(glyphs[cp])

    file_size = os.path.getsize(output_path)
    idx_size = count * 4
    bmp_size = count * 32

    print(f"Generated: {output_path}")
    print(f"  Glyphs:     {count}")
    print(f"  Index size:  {idx_size:,} bytes ({idx_size/1024:.1f} KB)")
    print(f"  Bitmap size: {bmp_size:,} bytes ({bmp_size/1024:.1f} KB)")
    print(f"  Total file:  {file_size:,} bytes ({file_size/1024:.1f} KB)")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <unifont.hex[.gz]> [output.bin]")
        print()
        print("Download Unifont hex from:")
        print("  https://unifoundry.com/pub/unifont/unifont-16.0.02/font-builds/unifont-16.0.02.hex.gz")
        sys.exit(1)

    hex_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else "spiffs_data/fonts/unifont_cjk.bin"

    print(f"Parsing {hex_path}...")
    glyphs = parse_hex_file(hex_path)
    print(f"  Found {len(glyphs)} glyphs in target ranges")

    if not glyphs:
        print("ERROR: No glyphs found. Check the hex file path.")
        sys.exit(1)

    write_mcfn(glyphs, output_path)
    print("Done!")


if __name__ == '__main__':
    main()
