# branchdb

An embedded, branchable key-value store for AI agents — with O(1) branching.

Fork the entire dataset in constant time and zero data copying, mutate one
branch, and the others are untouched. The data lives in a persistent immutable
tree whose branches share every unchanged node (structural sharing), the same
idea Git uses to store commits.

## Why

AI agents explore: a coding agent tries several fixes at once, a tree-search
agent keeps thousands of partial trajectories alive, a simulation forks
timelines to compare outcomes. They all want the same thing — try many branches
cheaply, keep the good ones, throw the rest away.

Server databases like Neon already do copy-on-write branching, but they cap
active branches (Neon at ~20) and pay a network round trip per request. branchdb
takes the embedded path: a library inside the agent's process, no server, no
network, and a single process that owns thousands of branches with no
coordination cost.

This is a learning-first project built in the open. Expect dead ends and wrong
turns to stay in the history.

## Quick start

```rust
use branchdb::BranchDB;

let mut db = BranchDB::new();
db.put("main", b"user:1".to_vec(), b"alice".to_vec())?;

// Fork 1000 branches off main — each is O(1), zero data copied.
for i in 0..1000 {
    db.fork("main", &format!("try-{i}"))?;
}

// Mutate exactly one branch.
db.put("try-7", b"user:1".to_vec(), b"bob".to_vec())?;

assert_eq!(db.get("main",  b"user:1")?, Some(&b"alice"[..])); // untouched
assert_eq!(db.get("try-7", b"user:1")?, Some(&b"bob"[..]));   // diverged
```

Persist to disk and reopen — shared nodes are written once:

```rust
db.save("agent.db")?;
let restored = branchdb::BranchDB::open("agent.db")?;
```

Run the demos and the tests:

```sh
cargo run --example demo
cargo run --example persistence
cargo test
```

## How it works

A branch is just a pointer to the root of a persistent tree (`Option<Arc<Node>>`).

- **Fork** clones the root `Arc` — O(1), copies no data. Both branches now share
  every node until one of them is written to.
- **Insert / remove** rebuild only the nodes on the path from the root to the
  change (path copying, O(log n) new nodes) and share every subtree they don't
  descend into. The old version stays valid; that is what makes branching free.
- The tree is **AVL-balanced**, so it stays O(log n) even under sorted inserts.
  Rotations are copy-on-write too — they rebuild a few nodes and share the rest.

The `structural_sharing_is_real` test proves it: after inserting into one side,
an untouched subtree is asserted to be the *same allocation* in both versions via
`Arc::ptr_eq`.

## Status

Working: persistent tree (`insert` / `get` / `remove` / `diff`), the `BranchDB`
API (`fork` / `put` / `get` / `delete` / `diff` / `merge`), on-disk persistence
(`save` / `open`) where shared nodes are stored once, and tests that prove
structural sharing — in memory and on disk.

Planned: Python/JS bindings so agent runtimes can use it directly, and a
benchmark that pits O(1) forking against full-copy cloning.

## License

MIT
