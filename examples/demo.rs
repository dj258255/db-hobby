//! BranchDB 맛보기: 한 브랜치에서 갈래 1000개를 포크하고, 그중 하나만 수정한 뒤
//! 나머지가 멀쩡한지 확인한다. 포크는 데이터가 아니라 포인터를 복제한다.
//!
//! 실행:  cargo run --example demo

use branchdb::BranchDB;

fn b(s: &str) -> Vec<u8> {
    s.as_bytes().to_vec()
}

fn show(db: &BranchDB, branch: &str, key: &str) {
    let value = db
        .get(branch, key.as_bytes())
        .unwrap()
        .map(|v| String::from_utf8_lossy(v).into_owned())
        .unwrap_or_else(|| "<none>".into());
    println!("  {branch:<8} {key} = {value}");
}

fn main() {
    let mut db = BranchDB::new();

    // main 브랜치에 사실 하나.
    db.put("main", b("user:1"), b("alice")).unwrap();

    // main에서 1000개 브랜치를 포크한다. 각 포크는 O(1)이고 데이터 복사가 0이다 —
    // 갈라지기 전까지 전부 main의 노드를 공유한다.
    for i in 0..1000 {
        db.fork("main", &format!("try-{i}")).unwrap();
    }

    // 정확히 한 브랜치만 수정한다.
    db.put("try-7", b("user:1"), b("bob")).unwrap();

    println!("1000개 브랜치를 포크하고 try-7만 수정한 뒤:");
    show(&db, "main", "user:1"); // 여전히 alice
    show(&db, "try-7", "user:1"); // bob
    show(&db, "try-42", "user:1"); // 여전히 alice (main과 공유)

    println!("\n총 브랜치 수: {}", db.branch_names().len());
    println!("포크 1000개를 떴지만, 공유 덕분에 새로 저장된 노드는 한 줌뿐이다.");

    // 에이전트 루프의 마지막 단계: 비교하고, 좋으면 채택한다.
    println!("\n[diff] main 대비 try-7이 바꾼 것:");
    for change in db.diff("main", "try-7").unwrap() {
        println!("  {change:?}");
    }

    db.merge("try-7", "main").unwrap(); // 좋은 갈래를 채택
    println!("\n[merge] try-7을 main에 채택한 뒤:");
    show(&db, "main", "user:1"); // 이제 main도 bob
}
