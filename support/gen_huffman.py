#!/usr/bin/env python3
"""Build Zaplink's fixed-width decode trees from ATSC A/65 Annex C data."""

import argparse
import hashlib
from pathlib import Path
import struct
import sys

SOURCE = Path(__file__).with_name("atsc_huffman_tables.txt")
EXPECTED = {
    "C5": (1940, "9f7e22007069ac341a7d470e670149ba77be3376b2ba20d91074ed75f5fd56f4"),
    "C7": (1782, "941213accd866ca1e9441c4379077a69632b7f1461c4bae18f22e835f25f2488"),
}


def source_tables():
    sections = {}
    section = None
    for line in SOURCE.read_text(encoding="ascii").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line in ("[C5]", "[C7]"):
            section = line[1:-1]
            if section in sections:
                raise ValueError(f"duplicate section {section}")
            sections[section] = bytearray()
        elif section is None:
            raise ValueError("hex bytes precede a section")
        else:
            sections[section].extend(bytes.fromhex(line))
    if sections.keys() != EXPECTED.keys():
        raise ValueError("expected C5 and C7 source sections")
    for name, data in sections.items():
        length, digest = EXPECTED[name]
        if len(data) != length or hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"{name} does not match the verified ATSC decode table")
    return sections


def parse_trees(data):
    roots = [int.from_bytes(data[i:i + 2], "big") for i in range(0, 256, 2)]
    if roots[0] != 256 or roots != sorted(set(roots)):
        raise ValueError("invalid tree root offsets")
    trees = []
    for context, start in enumerate(roots):
        end = roots[context + 1] if context < 127 else len(data)
        if end > len(data) or end <= start or (end - start) % 2:
            raise ValueError(f"invalid tree boundary for context {context}")
        count = (end - start) // 2
        tree = []
        for node in range(count):
            children = []
            for child in data[start + node * 2:start + node * 2 + 2]:
                if child < 128:
                    if child >= count:
                        raise ValueError(f"invalid child offset in context {context}")
                    children.append(child)
                else:
                    children.append(-((child & 127) + 1))
            tree.append(tuple(children))
        # Reject cycles and unreachable nodes in the source table.
        visiting, visited = set(), set()

        def visit(node):
            if node in visiting:
                raise ValueError(f"cyclic tree in context {context}")
            if node in visited:
                return
            visiting.add(node)
            for child in tree[node]:
                if child >= 0:
                    visit(child)
            visiting.remove(node)
            visited.add(node)

        visit(0)
        if len(visited) != count:
            raise ValueError(f"unreachable nodes in context {context}")
        trees.append(tree)
    return trees


def build():
    sources = source_tables()
    title = parse_trees(sources["C5"])
    desc = parse_trees(sources["C7"])
    width = max(len(tree) for tree in title + desc)
    block_size = 128 * width * 4
    result = bytearray(struct.pack("<4sIIII", b"ATHU", 1, 20, 20 + block_size, width))
    for table in (title, desc):
        for tree in table:
            for left, right in tree + [(-1, -1)] * (width - len(tree)):
                result.extend(struct.pack("<hh", left, right))
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify the existing asset against verified source tables")
    parser.add_argument("--output", type=Path, default=Path("huffman.bin"))
    args = parser.parse_args()
    try:
        expected = build()
        if args.check:
            if not args.output.is_file() or args.output.read_bytes() != expected:
                raise ValueError(f"{args.output} is missing or differs from verified ATSC tables")
        else:
            args.output.write_bytes(expected)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Huffman table error: {error}\n")
    print(f"{'Verified' if args.check else 'Generated'} {args.output} ({len(expected)} bytes)")


if __name__ == "__main__":
    main()
