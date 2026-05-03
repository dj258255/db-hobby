# db-hobby

A small relational database written from scratch in C, built to dissect how
PostgreSQL and MySQL actually work inside. It goes from raw fixed-size pages all
the way up to running SQL: page storage, a buffer pool, a heap, a B+Tree index,
a hand-written SQL parser and executor, a write-ahead log, and transactions.

This is a learning project. The goal isn't to invent something new; it's to
reproduce the real structure accurately and understand it. Every layer is
covered by tests (396 checks across 24 suites).

![db-hobby REPL demo](docs/demo.svg)

## Quick start

```sh
make test            # build and run the test suite
make bench           # build (-O2) and run the benchmark (index vs scan, fsync cost)
make repl            # build the REPL
./build/db-hobby my.db # open (or create) a database and type SQL
```

A session:

```
db-hobby> CREATE TABLE users (id INT, name TEXT);
테이블 'users' 생성됨 (컬럼 2개)
  (인덱스: id 컬럼)
db-hobby> INSERT INTO users VALUES (1, 'kim');
db-hobby> INSERT INTO users VALUES (2, 'lee');
db-hobby> SELECT * FROM users WHERE id = 2;
id | name
2 | lee
(1행, 인덱스 사용)
db-hobby> BEGIN;
db-hobby> DELETE FROM users WHERE id = 1;
db-hobby> ROLLBACK;
db-hobby> SELECT * FROM users;
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
| `lock.c` | 2PL table locks (S/X) + deadlock detection (wait-for graph) for isolation | PG/InnoDB lock manager |

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
dirty pages -- for both the heap and the B+Tree index -- and logs them with a
commit marker + `fsync`. That log `fsync` is the **only durability point
(no-force)**: pages are then written back to the data file without `fsync`, and
the log is *not* truncated -- it accumulates committed history as the source of
truth, trimmed by a size-threshold checkpoint (and on every reopen). If a
transaction's dirty pages outgrow the buffer pool, they are **stolen** (evicted to
disk before commit) only after their *before-image* is logged first, so the change
stays undoable. Crash recovery on reopen walks the log: **redo** each committed
segment's after-images in commit order, then **undo** the uncommitted tail's
before-images and truncate any pages it allocated. Rollback undoes the same way.

See `DESIGN.md` for the full layer map and build order.

## Scope (honest limitations)

Kept simple on purpose: the first column of each table is treated as a unique
integer primary key; `WHERE` is in disjunctive normal form (AND-groups joined by
OR, no parentheses); joins are INNER only, each `ON` is a single `=`, chained up
to 4 tables (`INNER` and `LEFT`, aliases supported, so self-joins work);
projection, aggregation, `GROUP BY`, and `HAVING` work over a single table or a
join result; `NULL` can be stored (nullable columns) or arise from `LEFT JOIN`, except the PK column; subqueries are
uncorrelated and single-table/single-column; execution is single-threaded, but
**multiple transactions interleave** through sessions (`SESSION n` switches the
current one, up to 8): **readers take no locks** -- a reader is never blocked by
a writer, it simply sees the old version through MVCC visibility, and an open
transaction reads from its **begin-time snapshot** (repeatable-read-style; plain
statements outside a transaction are read-committed). Writers take table-level
`X` locks (strict 2PL, held to commit), so a second writer on the same table is
rejected immediately -- **first-updater-wins at table granularity**. That same
X lock guarantees one writer per table, which is exactly why the per-table WAL
recovery (steal/undo/no-force) still holds unchanged. The PK index is
**multi-version** (one entry per row version, like PostgreSQL); lookups pick the
visible version. Closing the database rolls back any open transactions, like a
real server dropping connections. The WAL
protects both the data (`.tbl`) and index
(`.idx`) files; a transaction larger than the buffer pool is handled by stealing
dirty pages to disk with undo (before-image) logging, and commit is **no-force**
(one log `fsync`; the log is the source of truth, trimmed by a simple size-threshold
checkpoint). Logging is whole-page physical -- redo *and* undo are idempotent, so
pageLSN and CLRs are unnecessary here (a crash-injection test proves recovery
converges even when re-crashed mid-undo); fuzzy checkpoints and 3-pass ARIES
await the preconditions that actually demand them (concurrency, physiological
logging). `DELETE` is MVCC-style: rows are never physically removed -- `DELETE`
(and `UPDATE`'s old version) just stamps `xmax`, and **every** read path (full
scan, PK point/range lookup, secondary index, joins, aggregates, subqueries)
filters by visibility; dead versions accumulate until **`VACUUM [table]`**
reclaims them: it deletes their index entries (lazy B+Tree leaf deletion,
PostgreSQL-nbtree-style -- no merge/redistribute), empties their heap slots,
compacts each page (RIDs stay stable), and truncates trailing all-empty pages
(PG-style conditional truncation). VACUUM refuses to run inside a transaction
(as in PostgreSQL), and its heap/index cleanup commits atomically through the
WAL. These are noted in the code where they matter.

## License

MIT
