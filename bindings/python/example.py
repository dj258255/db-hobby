"""branchdb 파이썬 바인딩 맛보기.

먼저 빌드:
    cd bindings/python
    pip install maturin
    maturin develop
그다음:
    python example.py
"""

from branchdb import BranchDB

db = BranchDB()
db.put("main", b"user:1", b"alice")

# main에서 갈래를 포크해 따로 시도한다 (O(1)).
db.fork("main", "experiment")
db.put("experiment", b"user:1", b"bob")

assert db.get("main", b"user:1") == b"alice"      # 원본 그대로
assert db.get("experiment", b"user:1") == b"bob"  # 갈래만 바뀜

# 무엇이 달라졌는지 확인하고, 좋으면 채택한다.
print("diff(main -> experiment):", db.diff("main", "experiment"))
db.merge("experiment", "main")
assert db.get("main", b"user:1") == b"bob"

# 디스크에 저장하고 다시 연다.
db.save("agent.db")
restored = BranchDB.open("agent.db")
assert restored.get("main", b"user:1") == b"bob"
print("branches:", restored.branch_names())
print("OK")
