# Third-party notices

The MIT license in [LICENSE](LICENSE) covers ZapLink's original code and
documentation. It does not replace the rights or terms for the material and
dependencies described below.

## ATSC Huffman table data

- Material: numeric C5/C7 decode tables in
  `support/atsc_huffman_tables.txt` and the corresponding table data transformed
  into the generated `huffman.bin` asset.
- Source: [ATSC A/65:2013](https://www.atsc.org/wp-content/uploads/2021/04/A65_2013.pdf),
  Annex C, tables C5 and C7; usage grant in footnote 19 on printed page 88.
- Copyright (c) 1997 General Instrument Corporation.

Footnote 19 permits royalty-free, unlimited use of tables C4 through C7 in
conjunction with the A/65 standard, while reserving other rights. ZapLink uses
the C5/C7 data for A/65 PSIP decoding. This data remains under that grant and is
not offered under ZapLink's MIT license. Retain this notice with distributions
of the table data or generated asset. The repository preserves the source URL,
PDF checksum, and table checksums for reproducibility; it does not bundle the
standard's PDF.

## SQLite

ZapLink links SQLite, and `support/setup_env.sh` can download and build the
SQLite amalgamation locally. The upstream SQLite deliverable code is dedicated
to the public domain, as described in the
[SQLite copyright statement](https://www.sqlite.org/copyright.html).
SQLite is not relicensed by ZapLink. Retain the upstream information supplied
with any SQLite sources or binaries you redistribute.

## Other runtime and system dependencies

FFmpeg, the dvbv5 tools from v4l-utils, and curl are invoked as external
programs. The application also depends on the system C and POSIX libraries.
These components retain their own licenses. Their source code and binaries are
not included in this repository. When packaging them with ZapLink, follow the
terms and preserve the notices accompanying those particular builds; ZapLink's
MIT license does not grant rights to those components.
