# branchdb (Python)

Python bindings for [branchdb](../../README.md) — an embedded, branchable
key-value store for AI agents with O(1) copy-on-write branching.

## Build

```sh
cd bindings/python
pip install maturin
maturin develop
```

## Use

```python
from branchdb import BranchDB

db = BranchDB()
db.put("main", b"user:1", b"alice")

db.fork("main", "experiment")          # O(1), zero copy
db.put("experiment", b"user:1", b"bob")

db.get("main", b"user:1")              # b"alice" (untouched)
db.get("experiment", b"user:1")        # b"bob"

db.diff("main", "experiment")          # [("modified", b"user:1", b"bob")]
db.merge("experiment", "main")         # adopt the branch
db.save("agent.db")                    # persist; reopen with BranchDB.open(...)
```

Keys and values are `bytes`. See `example.py` for a runnable walkthrough.
