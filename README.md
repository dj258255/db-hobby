# minidb

A small relational database written from scratch in C, built to dissect how
PostgreSQL and MySQL actually work inside. It goes from raw fixed-size pages all
the way up to running SQL: page storage, a buffer pool, a heap, a B+Tree index,
a hand-written SQL parser and executor, a write-ahead log, and transactions.

This is a learning project. The goal isn't to invent something new; it's to
reproduce the real structure accurately and understand it. Every layer is
covered by tests (110 checks across 10 suites).

![minidb REPL demo](docs/demo.svg)

## Quick start

```sh
make test            # build and run the test suite
make repl            # build the REPL
./build/minidb my.db # open (or create) a database and type SQL
```

A session:

```
minidb> CREATE TABLE users (id INT, name TEXT);
테이블 'users' 생성됨 (컬럼 2개)
  (인덱스: id 컬럼)
minidb> INSERT INTO users VALUES (1, 'kim');
minidb> INSERT INTO users VALUES (2, 'lee');
minidb> SELECT * FROM users WHERE id = 2;
id | name
2 | lee
(1행, 인덱스 사용)
minidb> BEGIN;
minidb> DELETE FROM users WHERE id = 1;
minidb> ROLLBACK;
minidb> SELECT * FROM users;
id | name
1 | kim
2 | lee
(2행)
```

Data is stored in a single file and survives a restart (the schema is persisted
too, so no need to re-run `CREATE TABLE`).

## What's inside

Built bottom-up; each layer sits on the one below it.

| Layer | What it does | Mirrors |
|---|---|---|
| `pager.c` | fixed-size 4KB pages <-> a single file (`page_id * PAGE_SIZE`) | SQLite pager, PG smgr |
| `page.c` | slotted page: pack variable-length rows into a page | PG/InnoDB page layout |
| `bufpool.c` | page cache with pin counts, dirty flags, LRU eviction | InnoDB buffer pool |
| `heap.c` | table = a collection of pages; rows addressed by `RID = (page, slot)` | PG heap |
| `sql.c` | hand-written lexer + recursive-descent parser (SQL -> AST) | every DB frontend |
| `db.c` | executor + tuple codec + catalog (schema persisted in page 0) | pg_catalog |
| `btree.c` | on-disk B+Tree index for O(log n) lookups, with node splits | InnoDB clustered index |
| `wal.c` | write-ahead log: durability and atomicity, with crash recovery | PG WAL / redo log |

## SQL supported

```
CREATE TABLE <t> (<col> INT|TEXT, ...)
INSERT INTO <t> VALUES (<int|'text'>, ...)
SELECT * FROM <t> [WHERE <col> <op> <value>]
UPDATE <t> SET <col> = <value> [WHERE <col> <op> <value>]
DELETE FROM <t> [WHERE <col> <op> <value>]
BEGIN | COMMIT | ROLLBACK

<op> is one of  =  !=  <  >  <=  >=
```

An `=` on the first column (an `INT` primary key) uses the B+Tree index for an
O(log n) point lookup; range and inequality conditions fall back to a full scan
-- the kind of choice a query planner makes. Transactions use a no-steal +
force-at-commit policy and roll back both the heap and the index.

See `DESIGN.md` for the full layer map and build order.

## Scope (honest limitations)

Kept simple on purpose: one table per database, the first column is treated as a
unique integer primary key, `WHERE` is a single `=` comparison, and there is no
isolation/concurrency (one transaction at a time). B+Tree deletion isn't
implemented (deleted rows are tombstoned in the heap, so a stale index entry is
harmless). These are noted in the code where they matter.

## License

MIT
