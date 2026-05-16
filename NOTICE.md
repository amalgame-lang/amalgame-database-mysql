# NOTICE — amalgame-database-mysql

## Authorship

Copyright 2026 Bastien Mouget. Original work — see
`runtime/Amalgame_Database_MySQL.h`.

Part of the Amalgame ecosystem
([github.com/amalgame-lang/Amalgame](https://github.com/amalgame-lang/Amalgame)).
External contributions are paused at the ecosystem level; see the
main repo's `CONTRIBUTING.md` for the policy.

AI tools (Anthropic Claude) were used during development. Per
the project's authorship policy, AI is treated as a tool, not a
co-author at law.

## Licence

Apache License 2.0. See `LICENSE` for the full text.

## Third-party content

**None vendored.** This package binds to libmariadb (the C
connector that's drop-in compatible with libmysqlclient and the
de-facto standard on every modern distro). libmariadb is provided
by the user's operating system at compile time (via
`libmariadb-dev` / `libmariadb-devel` / `mariadb-connector-c`)
and at run time (via `libmariadb3` / `libmariadb.so` /
`libmariadb.dylib` / `libmariadb.dll`).

libmariadb itself is distributed under the
[GNU LGPL 2.1](https://github.com/mariadb-corporation/mariadb-connector-c/blob/master/COPYING.LIB)
— dynamic-linking against an LGPL library is explicitly permitted
for Apache-2.0 consumers (no copyleft virality through dynamic
linking; the LGPL §5 / §6 exception covers this exact case).
libmysqlclient (the Oracle-shipped equivalent) is GPL-2.0 with
FOSS exception; the dynamic-link path stays compatible too.

This package does not include or redistribute any libmariadb or
libmysqlclient code; users obtain those independently from their
OS package manager or from upstream.

## Trademarks

"MySQL" is a trademark of Oracle Corporation. "MariaDB" is a
trademark of MariaDB Foundation. This repository uses both names
solely to identify the database engines the package binds to.
No trademark claim is asserted.
