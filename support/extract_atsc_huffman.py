#!/usr/bin/env python3
"""Reproduce numeric C5/C7 bytes from the official ATSC A/65:2013 PDF.

Requires pdftotext (Poppler). The PDF and table hashes prevent silently using a
revised document or an incomplete text extraction.
"""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess

PDF_SHA256 = "2b3197e6c5a9ff32059d11ca1e35eae1e6d8a87d4ee31b9630c715f8823d102a"
HEADER = """# ATSC A/65:2013, Annex C, Tables C5 (title) and C7 (description).
# Source: https://www.atsc.org/wp-content/uploads/2021/04/A65_2013.pdf
# Source PDF SHA-256: 2b3197e6c5a9ff32059d11ca1e35eae1e6d8a87d4ee31b9630c715f8823d102a
# Extracted with pdftotext -layout, reassembled by decimal byte index.
# C7 byte 1781 prints as "15" plus a wrapped "5" on the next line; restored as 155.
# The standard grants unlimited royalty-free use of C4-C7 with A/65 (Annex C footnote 19).
# This file contains only decode-table numeric data, not the PDF text or layout.
"""
TABLES = (
    ("C5", "Table C5 English-language Program Title Decode Table", "C.4 STANDARD COMPRESSION TYPE 2", 1940,
     "9f7e22007069ac341a7d470e670149ba77be3376b2ba20d91074ed75f5fd56f4"),
    ("C7", "Table C7 English-language Program Description Decode Table", "D.1 INTRODUCTION", 1782,
     "941213accd866ca1e9441c4379077a69632b7f1461c4bae18f22e835f25f2488"),
)


def extract(pdf):
    if hashlib.sha256(pdf.read_bytes()).hexdigest() != PDF_SHA256:
        raise ValueError("PDF checksum does not match ATSC A/65:2013 source")
    text = subprocess.run(["pdftotext", "-layout", str(pdf), "-"],
                          check=True, capture_output=True, text=True).stdout
    output = HEADER
    for name, heading, next_heading, length, digest in TABLES:
        start = text.rfind(heading)
        end = text.find(next_heading, start + len(heading))
        if start < 0 or end < 0:
            raise ValueError(f"{name} table boundaries missing")
        pairs = []
        for line in text[start:end].splitlines():
            found = re.findall(r"(?<!\d)(\d+)\s+(\d+)(?!\d)", line)
            if len(found) >= 3 and line.lstrip()[:1].isdigit():
                pairs.extend((int(index), int(value)) for index, value in found)
        values = dict(pairs)
        if len(pairs) != length or set(values) != set(range(length)):
            raise ValueError(f"{name} byte indices are incomplete or duplicated")
        if name == "C7":
            if values[1781] != 15:
                raise ValueError("C7 wrapped final byte differs from published PDF")
            values[1781] = 155
        data = bytes(values[index] for index in range(length))
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"{name} data checksum mismatch")
        output += f"[{name}]\n"
        output += "".join(data[i:i + 16].hex(" ") + "\n" for i in range(0, length, 16))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pdf", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("atsc_huffman_tables.txt"))
    args = parser.parse_args()
    try:
        args.output.write_text(extract(args.pdf), encoding="ascii")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Table extraction failed: {error}\n")


if __name__ == "__main__":
    main()
