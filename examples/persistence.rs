//! 디스크 저장/복원 맛보기: 저장하고, 프로그램을 "껐다 켠 셈 치고" 다시 열어서
//! 데이터가 살아있는지 확인한다. 또 공유 노드가 디스크에서도 한 번만 저장됨을
//! 파일 크기로 보여준다.
//!
//! 실행:  cargo run --example persistence

use branchdb::BranchDB;

fn b(s: &str) -> Vec<u8> {
    s.as_bytes().to_vec()
}

fn file_size(path: &str) -> u64 {
    std::fs::metadata(path).map(|m| m.len()).unwrap_or(0)
}

fn main() {
    let path = std::env::temp_dir().join("branchdb_demo.db");
    let path = path.to_str().unwrap();

    // 1) 만들고, 포크하고, 한 갈래만 수정한 뒤 저장한다.
    let mut db = BranchDB::new();
    db.put("main", b("user:1"), b("alice")).unwrap();
    db.fork("main", "exp").unwrap();
    db.put("exp", b("user:1"), b("bob")).unwrap();
    db.save(path).unwrap();
    println!("저장 완료 -> {path} ({} bytes)", file_size(path));

    // 2) 프로그램을 새로 켠 셈 치고 디스크에서만 복원한다.
    let restored = BranchDB::open(path).unwrap();
    println!("\n다시 연 뒤:");
    println!("  main user:1 = {}", read(&restored, "main", "user:1"));
    println!("  exp  user:1 = {}", read(&restored, "exp", "user:1"));
    println!("  브랜치들: {:?}", restored.branch_names());

    // 3) 공유 증명: 같은 트리를 1000번 포크해도 파일은 거의 안 커진다.
    let mut many = BranchDB::new();
    for kv in ["a", "b", "c", "d", "e"] {
        many.put("main", b(kv), b(kv)).unwrap();
    }
    let solo = std::env::temp_dir().join("branchdb_solo.db");
    many.save(solo.to_str().unwrap()).unwrap();
    let size_solo = file_size(solo.to_str().unwrap());

    for i in 0..1000 {
        many.fork("main", &format!("f{i}")).unwrap();
    }
    many.save(path).unwrap();
    println!(
        "\n브랜치 1개일 때 {size_solo} bytes -> 1001개여도 {} bytes",
        file_size(path)
    );
    println!("트리는 한 번만 저장되고, 늘어난 건 브랜치 이름표뿐이다.");

    let _ = std::fs::remove_file(path);
    let _ = std::fs::remove_file(solo);
}

fn read(db: &BranchDB, branch: &str, key: &str) -> String {
    db.get(branch, key.as_bytes())
        .unwrap()
        .map(|v| String::from_utf8_lossy(v).into_owned())
        .unwrap_or_else(|| "<none>".into())
}
