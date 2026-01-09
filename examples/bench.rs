//! 이 데이터베이스가 왜 존재하는지를 숫자로 보여주는 벤치마크.
//!
//! 같은 작업 — "데이터셋을 1000개의 독립된 갈래로 나누기" — 를 두 방식으로 잰다.
//!   1) BranchDB::fork  — 루트 포인터만 복제(O(1), 구조 공유)
//!   2) 순진한 전체 복사 — 갈래마다 데이터 전체를 새로 만든다(구조 공유 없음, 파일
//!      복사로 포크하는 것과 같은 비용)
//!
//! 데이터셋 크기를 키워가며 잰다. fork는 크기와 무관하게 일정하고, 전체 복사는
//! 데이터가 커질수록 선형으로 느려진다.
//!
//! 실행:  cargo run --release --example bench

use std::time::Instant;

use branchdb::tree::{self, Tree};
use branchdb::BranchDB;

const BRANCHES: usize = 1000;

fn main() {
    println!(
        "{:>10} | {:>14} | {:>18} | {:>9}",
        "데이터셋", "fork ×1000", "전체 복사 ×1000", "배속"
    );
    println!("{}", "-".repeat(62));

    // 10,000까지만 잰다(전체 복사 10k가 이미 ~10초다). 100,000에서는 fork가 여전히
    // ~220µs인 반면 전체 복사는 ~130초로, 격차가 60만 배까지 벌어진다.
    for &n in &[100usize, 1_000, 10_000] {
        let mut db = BranchDB::new();
        for i in 0..n {
            db.put("main", format!("key:{i:08}").into_bytes(), b"v".to_vec())
                .unwrap();
        }

        // 1) fork: 루트 Arc만 복제.
        let t0 = Instant::now();
        for i in 0..BRANCHES {
            db.fork("main", &format!("f{i}")).unwrap();
        }
        let fork_ns = t0.elapsed().as_nanos().max(1);

        // 2) 순진한 복사: 갈래마다 전체 데이터를 새 트리로 다시 만든다.
        let entries = db.entries("main").unwrap();
        let t1 = Instant::now();
        let mut guard = 0usize;
        for _ in 0..BRANCHES {
            let mut copy: Tree = tree::empty();
            for (key, value) in &entries {
                copy = tree::insert(&copy, key.clone(), value.clone());
            }
            guard += copy.is_some() as usize; // 최적화로 사라지지 않게.
        }
        let copy_ns = t1.elapsed().as_nanos().max(1);
        std::hint::black_box(guard);

        let speedup = copy_ns as f64 / fork_ns as f64;
        println!(
            "{:>10} | {:>14} | {:>18} | {:>8.0}x",
            n,
            fmt(fork_ns),
            fmt(copy_ns),
            speedup
        );
    }

    println!("\nfork은 데이터 크기와 무관하게 일정하다.");
    println!("전체 복사는 데이터가 10배 커지면 대략 10배 느려진다 — 포크가 흔한 워크로드라면 치명적이다.");
}

/// 나노초를 읽기 좋은 단위로.
fn fmt(ns: u128) -> String {
    if ns < 1_000 {
        format!("{ns} ns")
    } else if ns < 1_000_000 {
        format!("{:.1} µs", ns as f64 / 1_000.0)
    } else if ns < 1_000_000_000 {
        format!("{:.1} ms", ns as f64 / 1_000_000.0)
    } else {
        format!("{:.2} s", ns as f64 / 1_000_000_000.0)
    }
}
