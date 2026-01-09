# minidb

A relational database built from scratch in C — to dissect how PostgreSQL and
MySQL actually work inside. Page storage, slotted pages, buffer pool, B-Tree,
SQL parser, executor — one layer at a time.

This is a learning project. The goal isn't to invent something new; it's to
reproduce the real structure accurately and understand it.

## Build & test

```sh
make test    # builds and runs the test suite
make clean
```

## Where things are

- `DESIGN.md` — the full layer map (parser → executor → catalog → storage) and build order.
- `src/pager.c` — layer 1: the disk manager (fixed-size pages ↔ a single file).

## Status

Building bottom-up:

1. **Pager / disk manager** — in progress
2. Slotted page
3. Buffer pool
4. Heap file
5. Catalog
6. SQL parser
7. Executor
8. B-Tree index
9. WAL / transactions

Goal:

```sql
CREATE TABLE users (id INT, name TEXT);
INSERT INTO users VALUES (1, 'kim');
SELECT * FROM users WHERE id = 1;
```

stored in a single file that survives a restart.

## License

MIT
