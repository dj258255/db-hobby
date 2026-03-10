# minidb

A small relational database written from scratch in C, built to dissect how
PostgreSQL and MySQL actually work inside. It goes from raw fixed-size pages all
the way up to running SQL: page storage, a buffer pool, a heap, a B+Tree index,
a hand-written SQL parser and executor, a write-ahead log, and transactions.

This is a learning project. The goal isn't to invent something new; it's to
reproduce the real structure accurately and understand it. Every layer is
covered by tests (302 checks across 18 suites).

![minidb REPL demo](docs/demo.svg)

## Quick start

```sh
make test            # build and run the test suite
make bench           # build (-O2) and run the benchmark (index vs scan, fsync cost)
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

Each table is stored as its own pair of files and survives a restart (the schema
is persisted too, so no need to re-run `CREATE TABLE`) -- see the storage layout
below.

## What's inside

Built bottom-up; each layer sits on the one below it.

| Layer | What it does | Mirrors |
|---|---|---|
| `pager.c` | fixed-size 4KB pages <-> a single file (`page_id * PAGE_SIZE`) | SQLite pager, PG smgr |
| `page.c` | slotted page: pack variable-length rows into a page | PG/InnoDB page layout |
| `bufpool.c` | page cache with pin counts, dirty flags, LRU eviction | InnoDB buffer pool |
| `heap.c` | table = a collection of pages; rows addressed by `RID = (page, slot)` | PG heap |
| `sql.c` | hand-written lexer + recursive-descent parser (SQL -> AST) | every DB frontend |
| `db.c` | executor: tuple codec, multi-table catalog, joins (NLJ/index/hash), aggregates | pg_catalog, executor |
| `btree.c` | on-disk B+Tree index for O(log n) lookups, with node splits | InnoDB clustered index |
| `wal.c` | write-ahead log on each table's data file: atomic commit + crash recovery | PG WAL / redo log |

### Storage layout

Like PostgreSQL (each relation is its own file, `relfilenode`), every table lives
in its own files, and a catalog file lists which tables exist:

```
mydb              catalog -- table names + schemas (like pg_class)
mydb.users.tbl    users rows  (heap)
mydb.users.wal    write-ahead log for the heap (commit atomicity + crash recovery)
mydb.users.idx    users PK index (B+Tree)
mydb.users.idx.wal  write-ahead log for the index
mydb.orders.tbl   orders rows
mydb.orders.wal
mydb.orders.idx   orders PK index
mydb.orders.idx.wal
```

## SQL supported

```
CREATE TABLE <t> (<col> INT|TEXT [NOT NULL], ...)
CREATE INDEX <name> ON <t> (<col>)   -- secondary index on an INT column
INSERT INTO <t> VALUES (<int|'text'>, ...)
SELECT [DISTINCT] <* | item, ...>
       FROM <t> [<alias>] [[LEFT] JOIN <t2> [<alias>] ON <colref> = <colref>]...
                  [WHERE <cond> [AND <cond>] [OR ...]]
                  [GROUP BY <col>] [HAVING <item> <op> <value>]
                  [ORDER BY <colref | position> [ASC|DESC], ...] [LIMIT <n>] [OFFSET <n>]
UPDATE <t> SET <col> = <value> [WHERE ...]
DELETE FROM <t> [WHERE ...]
BEGIN | COMMIT | ROLLBACK
EXPLAIN <select>          -- print the query plan instead of running it

<item>   is  <col> | COUNT(*) | COUNT|SUM|MIN|MAX|AVG(<col>)
<cond>   is  <colref> <op> <value>  |  <colref> <op> (SELECT <col> FROM <t> [WHERE ...])
                                   |  <colref> IS [NOT] NULL
                                   |  <colref> [NOT] IN (<value>, ...)
                                   |  <colref> [NOT] IN (SELECT <col> FROM <t> [WHERE ...])
                                   |  <colref> [NOT] BETWEEN <value> AND <value>
                                   |  <colref> [NOT] LIKE '<pattern>'   (% = any run, _ = one char)
<op>     is one of  =  !=  <  >  <=  >=
<colref> is  [<table>.]<col>
```

An `=`, `<`, `>`, `<=`, or `>=` on the first column (an `INT` primary key) uses
the B+Tree index -- `=` is an O(log n) point lookup, the others walk the linked
leaf chain as a range scan. A `CREATE INDEX` on a non-PK `INT` column adds a
secondary (non-unique) B+Tree; an `=` filter on that column does an index scan
(`btree_find_all` collects candidate RIDs, then each is heap-fetched and the
`WHERE` is rechecked to drop deleted/stale rows), and `EXPLAIN` shows `Index Scan
using <name>`. `!=`, other conditions, and compound `AND` conditions fall back to
a full scan -- the kind of choice a query planner makes. `ORDER BY`/`LIMIT` and `GROUP BY`/aggregates take a materialize path
(collect, then sort / sort-group); grouped results can be filtered with `HAVING`
and ordered by an output column or position (so `ORDER BY 2 DESC` gives top-N by
an aggregate). `JOIN` is a recursive N-way join that picks a
method per level like an optimizer: index nested-loop (`btree_search`) when the
inner's primary key is the `ON` key, hash join (build a hash on the inner's join
column, then O(1) probe) for any other equi-join, else a plain nested-loop scan.
`LEFT JOIN` preserves unmatched left rows by filling the right side with `NULL`.
`NULL` can also be stored: `INSERT ... VALUES (1, NULL)` keeps it via a null bitmap
at the front of each row (the first/PK column stays `NOT NULL`). Either way `COUNT(*)`
counts those rows but `COUNT(col)`/`SUM`/`AVG` skip the `NULL`s, and `IS [NOT] NULL` tests
for them (`LEFT JOIN ... WHERE right.col IS NULL` is the anti-join). `SELECT
DISTINCT` dedupes output rows. `IN (1, 2, 3)` tests membership against a literal
value set; `IN (SELECT ...)` runs an uncorrelated subquery once into a value set,
then tests membership the same way. `BETWEEN a AND b` is desugared at
parse time into `>= a AND <= b` (inclusive); `LIKE`/`NOT LIKE` match `%` (any run)
and `_` (one char) with a backtracking matcher -- both run as a full scan, not via
the index. Writes go through a **write-ahead
log**: a commit (explicit or per-statement autocommit) stages the transaction's
dirty pages -- for both the heap and the B+Tree index -- logs them with a commit
marker + `fsync`, and only then applies them to the file, so a crash mid-commit is
recovered on reopen (redo if the marker is present, discard if not). Rollback
discards dirty pages and truncates the files.

See `DESIGN.md` for the full layer map and build order.

## Scope (honest limitations)

Kept simple on purpose: the first column of each table is treated as a unique
integer primary key; `WHERE` is in disjunctive normal form (AND-groups joined by
OR, no parentheses); joins are INNER only, each `ON` is a single `=`, chained up
to 4 tables (`INNER` and `LEFT`, aliases supported, so self-joins work);
projection, aggregation, `GROUP BY`, and `HAVING` work over a single table or a
join result; `NULL` can be stored (nullable columns) or arise from `LEFT JOIN`, except the PK column; subqueries are
uncorrelated and single-table/single-column; execution is single-threaded, but
interleaved transactions are isolated by 2PL **table-level** locks (writes take an
`X` lock, reads an `S` lock, held until commit/rollback), and a conflict is
rejected rather than blocked on since there are no threads to wait; the WAL
protects both the data (`.tbl`) and index
(`.idx`) files, and a transaction's dirty pages must fit in the buffer pool. B+Tree
deletion isn't implemented (deleted rows are tombstoned in the heap, so a stale
index entry is harmless). These are noted in the code where they matter.

## License

MIT
