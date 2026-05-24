# amalgame-database-mysql

MySQL / MariaDB binding for [Amalgame](https://github.com/amalgame-lang/Amalgame).
Dynamic-linked to the system **libmariadb** — the C connector
that's drop-in compatible with `libmysqlclient` and the de-facto
standard on every modern distro. Sibling of
[`amalgame-database-postgresql`](https://github.com/amalgame-lang/amalgame-database-postgresql);
same dynamic-link manifest pattern.

Works against both **MariaDB ≥ 10.x** and **MySQL ≥ 5.7** — the
wire protocol is shared, so the C client handles both transparently.

## Prerequisites

Install the libmariadb development package on your build machine:

| OS / distro | Command |
|---|---|
| Debian / Ubuntu | `sudo apt install libmariadb-dev` |
| Fedora / RHEL / Rocky | `sudo dnf install mariadb-connector-c-devel` |
| Arch / Manjaro | `sudo pacman -S mariadb-libs` |
| Alpine | `apk add mariadb-connector-c-dev` |
| macOS (Homebrew) | `brew install mariadb-connector-c` |
| Windows (MSYS2) | `pacman -S mingw-w64-x86_64-libmariadbclient` |
| Windows (vanilla) | install MariaDB Connector/C from mariadb.com/downloads/ |

On the **deploy** machine you need the runtime variant:
`libmariadb3` on Debian/Ubuntu, `mariadb-connector-c` on Fedora,
etc. Often pulled in as a dependency of the database server
itself; install the client-only variant if you only need the
shared lib without the server.

## Install

```bash
amc package add mysql                                              # via index
amc package add github.com/amalgame-lang/amalgame-database-mysql@v0.2.0
```

Requires **amc 0.8.40+** (for `returns_generic` on QueryAll).

## Surface

```amalgame
import Amalgame.Database.MySQL

let db = MySQL.Open("127.0.0.1", 3306, "app", "s3cret", "mydb")
if (!MySQL.IsOpen(db)) {
    Console.WriteLine("connect failed: " + MySQL.LastError(db))
    return
}

MySQL.Exec(db, "CREATE TABLE IF NOT EXISTS notes (id INT AUTO_INCREMENT PRIMARY KEY, body TEXT)")
MySQL.Exec(db, "INSERT INTO notes (body) VALUES ('hello mysql')")
Console.WriteLine("inserted id=" + String_FromInt(MySQL.LastInsertId(db)))

let rows = MySQL.QueryAll(db, "SELECT id, body FROM notes ORDER BY id")
let i: int = 0
while (i < rows.Count()) {
    let row = rows.Get(i)
    Console.WriteLine(row.Get(0) + ": " + row.Get(1))
    i = i + 1
}

MySQL.Close(db)
```

### v0.1.0 method surface

| Method | Returns | Notes |
|---|---|---|
| `MySQL.Open(host, port, user, password, database)` | `AmalgameMySQL*` | All 5 args mandatory; pass `""` for empty user/password/db |
| `MySQL.Close(db)` | `void` | Idempotent; GC also closes leaked handles |
| `MySQL.IsOpen(db)` | `bool` | Live connection check |
| `MySQL.LastError(db)` | `string` | Empty on success |
| `MySQL.Exec(db, sql)` | `bool` | DDL / INSERT / UPDATE / DELETE; updates Changes() + LastInsertId() |
| `MySQL.QueryAll(db, sql)` | `List<List<string>>` | SELECT, text mode |
| `MySQL.Changes(db)` | `int` | Rows affected by last Exec / row count of last QueryAll |
| `MySQL.LastInsertId(db)` | `int` | AUTO_INCREMENT value of last INSERT |
| `MySQL.ServerVersion(db)` | `string` | "10.11.6-MariaDB", "8.0.35", … |

### Connection params

Unlike PostgreSQL's connection-string idiom, libmariadb takes the
five params individually. Defaults that work against a stock
docker-compose:

```amalgame
let db = MySQL.Open("127.0.0.1", 3306, "root", "test", "amctest")
```

Use `""` (empty string) for password if the server is
unauthenticated, and likewise for database if you want the
default schema.

## Pixel layout / data model

The v1 surface stringifies every cell via `mysql_fetch_row +
mysql_fetch_lengths` (libmariadb text mode, length-prefixed so
columns with embedded NULs round-trip cleanly). NULL cells
materialise as the empty string — callers that need to
distinguish `NULL` from `''` should use parameter binding +
typed accessors in v2.

## Deferred to v2

- `mysql_stmt_*` prepared statements + `?` parameter binding
- Typed column accessors (`AsInt(col)`, `AsBytes(col)`, `AsTimestamp(col)`)
- Multi-statement results (`CLIENT_MULTI_STATEMENTS` flag)
- Async query mode (`MYSQL_OPT_NONBLOCK`)
- `LOAD DATA INFILE` / streaming bulk loaders
- SSL/TLS-specific control surface (libmariadb already honours
  `MYSQL_OPT_SSL_*` via env vars today — explicit AM API in v2)
- Stored-procedure OUT/INOUT parameters

## Threading

`AmalgameMySQL*` is single-owner. Concurrent `Exec` / `QueryAll`
calls against the same handle from different threads will
interleave wire bytes — libmariadb is **not** internally
thread-safe for shared connections. Use distinct handles per
thread. Async / pipelined query lands in v2.

## Tests

```bash
./tests/run_tests.sh /path/to/amc
```

Double-gated runner. Both `libmariadb-dev` AND a reachable
MySQL/MariaDB on `127.0.0.1:3306` must be present, else every
case SKIPs cleanly. Start a server locally with:

```bash
docker run --rm -d --name mysqltest -p 3306:3306 \
  -e MYSQL_ROOT_PASSWORD=test -e MYSQL_DATABASE=amctest \
  mysql:8
```

then export:

```bash
export MYSQL_HOST=127.0.0.1 MYSQL_PORT=3306
export MYSQL_USER=root MYSQL_PASSWORD=test MYSQL_DATABASE=amctest
```

before running the suite.

## Licence

Apache-2.0 — see [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).
libmariadb itself is LGPL-2.1; dynamic-linking against an LGPL
library is explicitly permitted for Apache-2.0 consumers (no
copyleft virality through dynamic linking).
