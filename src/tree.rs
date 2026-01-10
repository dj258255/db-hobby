//! 영속(불변) 순서 키-값 트리 — AVL 균형.
//!
//! "영속(persistent)"은 자료구조 용어다. 모든 수정이 옛 버전을 그대로 살려둔 채
//! *새* 버전을 반환한다. 제자리에서 바뀌는 것은 아무것도 없다. 이 성질 덕분에
//! 브랜치가 공짜가 된다 — 브랜치는 루트 노드를 가리키는 포인터 하나이고, 두
//! 브랜치는 둘 다 건드리지 않은 노드를 전부 공유한다.
//!
//! 트리는 AVL 규칙으로 균형을 유지한다(좌우 높이 차 ≤ 1). 그래서 정렬된 키를
//! 순서대로 넣어도 최악 O(n)으로 무너지지 않고 O(log n)을 지킨다. 회전(rotation)
//! 역시 copy-on-write다 — 관련된 몇 노드만 새로 만들고 나머지 부분 트리는 공유한다.

use std::cmp::Ordering;
use std::sync::Arc;

/// 키와 값은 그냥 바이트 덩어리다 — 실제 KV 저장소처럼 내용은 신경 쓰지 않는다.
pub type Key = Vec<u8>;
pub type Value = Vec<u8>;

/// 트리의 노드 하나. 불변이다: 한 번 만들어지면 필드는 절대 바뀌지 않는다.
///
/// `left`와 `right`는 `Tree`(즉 `Option<Arc<Node>>`)다. `Arc`는 원자적 참조 카운팅
/// 포인터로, 복제해도 데이터를 복사하지 않고 카운터만 1 올린다 — 구조 공유의 비밀이다.
/// `height`는 AVL 균형을 위한 이 노드의 높이다(자식에서 유도되므로 디스크엔 저장하지 않는다).
#[derive(Debug)]
pub struct Node {
    pub key: Key,
    pub value: Value,
    pub left: Tree,
    pub right: Tree,
    pub height: u32,
}

/// 트리 전체(또는 부분 트리)는 그냥 루트 노드를 가리키는 선택적 공유 포인터다.
/// `None`이 빈 트리다. `Tree`의 복제는 O(1)이다.
pub type Tree = Option<Arc<Node>>;

/// 트리의 높이. 빈 트리는 0.
pub fn height(tree: &Tree) -> u32 {
    tree.as_ref().map_or(0, |n| n.height)
}

/// 자식으로부터 높이를 계산해 노드를 만든다(균형은 보장하지 않는 원시 생성자).
fn make(key: Key, value: Value, left: Tree, right: Tree) -> Arc<Node> {
    let height = 1 + height(&left).max(height(&right));
    Arc::new(Node { key, value, left, right, height })
}

/// 균형 계수: 왼쪽 높이 - 오른쪽 높이. +면 왼쪽이 무겁다.
fn balance_factor(node: &Node) -> i32 {
    height(&node.left) as i32 - height(&node.right) as i32
}

/// 오른쪽 회전. 왼쪽 자식을 새 루트로 끌어올린다. 관련 노드만 새로 만들고
/// 나머지 부분 트리(`l.left`, `node.right`)는 그대로 공유한다.
fn rotate_right(node: &Node) -> Arc<Node> {
    let l = node.left.as_ref().expect("rotate_right는 왼쪽 자식이 필요하다");
    let new_right = make(node.key.clone(), node.value.clone(), l.right.clone(), node.right.clone());
    make(l.key.clone(), l.value.clone(), l.left.clone(), Some(new_right))
}

/// 왼쪽 회전. `rotate_right`의 거울상.
fn rotate_left(node: &Node) -> Arc<Node> {
    let r = node.right.as_ref().expect("rotate_left는 오른쪽 자식이 필요하다");
    let new_left = make(node.key.clone(), node.value.clone(), node.left.clone(), r.left.clone());
    make(r.key.clone(), r.value.clone(), Some(new_left), r.right.clone())
}

/// 노드를 만들되 AVL 규칙이 깨졌으면 회전으로 바로잡는다. 삽입·삭제는 한 번에 한
/// 노드만 균형을 깨므로, 경로를 거슬러 올라오며 이걸 호출하면 트리 전체가 균형을 되찾는다.
fn rebalance(key: Key, value: Value, left: Tree, right: Tree) -> Arc<Node> {
    let node = make(key, value, left, right);
    let bf = balance_factor(&node);

    if bf > 1 {
        // 왼쪽이 무겁다.
        let l = node.left.as_ref().unwrap();
        if balance_factor(l) < 0 {
            // 왼쪽-오른쪽: 왼쪽 자식을 먼저 왼쪽 회전한 뒤 오른쪽 회전.
            let new_left = rotate_left(l);
            let node = make(node.key.clone(), node.value.clone(), Some(new_left), node.right.clone());
            rotate_right(&node)
        } else {
            rotate_right(&node)
        }
    } else if bf < -1 {
        // 오른쪽이 무겁다.
        let r = node.right.as_ref().unwrap();
        if balance_factor(r) > 0 {
            // 오른쪽-왼쪽.
            let new_right = rotate_right(r);
            let node = make(node.key.clone(), node.value.clone(), node.left.clone(), Some(new_right));
            rotate_left(&node)
        } else {
            rotate_left(&node)
        }
    } else {
        node
    }
}

/// 빈 트리.
pub fn empty() -> Tree {
    None
}

/// 키를 조회한다. 저장된 값의 참조를 반환하거나, 없으면 `None`.
pub fn get<'a>(tree: &'a Tree, key: &[u8]) -> Option<&'a [u8]> {
    let mut cur = tree;
    while let Some(node) = cur {
        match key.cmp(&node.key) {
            Ordering::Less => cur = &node.left,
            Ordering::Greater => cur = &node.right,
            Ordering::Equal => return Some(&node.value),
        }
    }
    None
}

/// 키를 삽입(또는 덮어쓰기)하고 *새* 트리를 반환한다. 입력 `tree`는 손대지 않는다.
///
/// 변경 지점까지의 경로만 다시 만드는 재귀다(경로 복사). 내려가지 않은 쪽 부분
/// 트리는 그대로 공유하고, 경로를 거슬러 올라오며 `rebalance`로 AVL 균형을 지킨다.
pub fn insert(tree: &Tree, key: Key, value: Value) -> Tree {
    Some(match tree {
        // 빈 자리: 새 리프.
        None => make(key, value, None, None),
        Some(node) => match key.cmp(&node.key) {
            Ordering::Less => rebalance(
                node.key.clone(),
                node.value.clone(),
                insert(&node.left, key, value),
                node.right.clone(),
            ),
            Ordering::Greater => rebalance(
                node.key.clone(),
                node.value.clone(),
                node.left.clone(),
                insert(&node.right, key, value),
            ),
            // 같은 키: 값만 바꾼다(구조가 안 변하니 회전 불필요).
            Ordering::Equal => make(node.key.clone(), value, node.left.clone(), node.right.clone()),
        },
    })
}

/// 부분 트리에서 가장 작은 (키, 값). BST에서 최솟값은 맨 왼쪽 노드다.
fn min_kv(tree: &Tree) -> (Key, Value) {
    let mut cur = tree.as_ref().expect("빈 트리에는 최솟값이 없다");
    while let Some(left) = cur.left.as_ref() {
        cur = left;
    }
    (cur.key.clone(), cur.value.clone())
}

/// 키를 삭제하고 *새* 트리를 반환한다. `insert`처럼 입력은 손대지 않으며, 경로를
/// 거슬러 올라오며 균형을 되찾는다.
///
/// 까다로운 경우는 "지울 노드에 자식이 둘 다 있을 때"다. 오른쪽 부분 트리의
/// 최솟값(후속자)을 끌어올려 그 자리를 채우고, 후속자를 오른쪽에서 지운다.
pub fn remove(tree: &Tree, key: &[u8]) -> Tree {
    match tree {
        None => None,
        Some(node) => match key.cmp(&node.key) {
            Ordering::Less => Some(rebalance(
                node.key.clone(),
                node.value.clone(),
                remove(&node.left, key),
                node.right.clone(),
            )),
            Ordering::Greater => Some(rebalance(
                node.key.clone(),
                node.value.clone(),
                node.left.clone(),
                remove(&node.right, key),
            )),
            Ordering::Equal => match (&node.left, &node.right) {
                (None, None) => None,
                (Some(_), None) => node.left.clone(),
                (None, Some(_)) => node.right.clone(),
                (Some(_), Some(_)) => {
                    let (succ_key, succ_value) = min_kv(&node.right);
                    let new_right = remove(&node.right, &succ_key);
                    Some(rebalance(succ_key, succ_value, node.left.clone(), new_right))
                }
            },
        },
    }
}

/// `a`에서 `b`로 갈 때 한 키에 일어난 변화.
#[derive(Debug, PartialEq, Eq)]
pub enum Change {
    /// `b`에만 있는 키.
    Added { key: Key, value: Value },
    /// `a`에만 있는 키.
    Removed { key: Key },
    /// 양쪽에 있지만 값이 다른 키.
    Modified { key: Key, old: Value, new: Value },
}

/// 트리를 키 순서대로 (키, 값) 목록으로 펼친다(중위 순회). BST의 중위 순회는
/// 항상 정렬된 순서를 준다 — `diff`가 이걸 활용한다.
pub fn entries(tree: &Tree) -> Vec<(Key, Value)> {
    fn go(node: &Tree, out: &mut Vec<(Key, Value)>) {
        if let Some(n) = node {
            go(&n.left, out);
            out.push((n.key.clone(), n.value.clone()));
            go(&n.right, out);
        }
    }
    let mut out = Vec::new();
    go(tree, &mut out);
    out
}

/// 두 트리의 차이를 키 순서로 반환한다. `a`에서 `b`로 갈 때의 변화다.
///
/// 양쪽을 정렬된 목록으로 펼친 뒤 두 포인터로 한 번에 훑는다(병합 비교) — O(n).
pub fn diff(a: &Tree, b: &Tree) -> Vec<Change> {
    let (ea, eb) = (entries(a), entries(b));
    let mut out = Vec::new();
    let (mut i, mut j) = (0, 0);

    while i < ea.len() && j < eb.len() {
        match ea[i].0.cmp(&eb[j].0) {
            Ordering::Less => {
                out.push(Change::Removed { key: ea[i].0.clone() });
                i += 1;
            }
            Ordering::Greater => {
                out.push(Change::Added { key: eb[j].0.clone(), value: eb[j].1.clone() });
                j += 1;
            }
            Ordering::Equal => {
                if ea[i].1 != eb[j].1 {
                    out.push(Change::Modified {
                        key: ea[i].0.clone(),
                        old: ea[i].1.clone(),
                        new: eb[j].1.clone(),
                    });
                }
                i += 1;
                j += 1;
            }
        }
    }
    while i < ea.len() {
        out.push(Change::Removed { key: ea[i].0.clone() });
        i += 1;
    }
    while j < eb.len() {
        out.push(Change::Added { key: eb[j].0.clone(), value: eb[j].1.clone() });
        j += 1;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn k(s: &str) -> Key {
        s.as_bytes().to_vec()
    }

    /// 트리가 AVL 불변(모든 노드에서 좌우 높이 차 ≤ 1)을 지키는지 검사한다.
    fn assert_balanced(tree: &Tree) {
        if let Some(node) = tree {
            assert!(balance_factor(node).abs() <= 1, "AVL 불변 위반");
            assert_balanced(&node.left);
            assert_balanced(&node.right);
        }
    }

    #[test]
    fn insert_then_get() {
        let t = empty();
        let t = insert(&t, k("b"), k("2"));
        let t = insert(&t, k("a"), k("1"));
        let t = insert(&t, k("c"), k("3"));

        assert_eq!(get(&t, b"a"), Some(&b"1"[..]));
        assert_eq!(get(&t, b"b"), Some(&b"2"[..]));
        assert_eq!(get(&t, b"c"), Some(&b"3"[..]));
        assert_eq!(get(&t, b"z"), None);
    }

    #[test]
    fn overwrite_same_key() {
        let t = empty();
        let t = insert(&t, k("a"), k("1"));
        let t = insert(&t, k("a"), k("99"));
        assert_eq!(get(&t, b"a"), Some(&b"99"[..]));
    }

    #[test]
    fn old_version_is_untouched() {
        // 정의적 성질: 수정은 새 트리를 반환하고 옛 트리는 그대로 유효하게 둔다.
        let v1 = insert(&empty(), k("a"), k("1"));
        let v2 = insert(&v1, k("a"), k("2"));

        assert_eq!(get(&v1, b"a"), Some(&b"1"[..])); // 옛 버전 그대로
        assert_eq!(get(&v2, b"a"), Some(&b"2"[..])); // 새 버전은 수정 반영
    }

    #[test]
    fn stays_balanced_under_sorted_inserts() {
        // 정렬된 키를 1000개 순서대로 넣는다 — 균형 안 잡힌 BST라면 높이 1000의
        // 사실상 연결 리스트가 된다. AVL이면 높이가 로그로 유지돼야 한다.
        let mut t = empty();
        for i in 0..1000 {
            t = insert(&t, format!("{i:05}").into_bytes(), k("v"));
        }
        assert_balanced(&t);
        // AVL 높이 상한은 약 1.44·log2(n) ≈ 15. 넉넉히 20 미만이면 균형이 맞다.
        assert!(height(&t) < 20, "트리가 균형을 잃었다: 높이 {}", height(&t));
        // 그래도 모든 키를 찾을 수 있어야 한다.
        for i in 0..1000 {
            assert_eq!(get(&t, format!("{i:05}").as_bytes()), Some(&b"v"[..]));
        }
    }

    #[test]
    fn remove_handles_all_child_counts_and_stays_balanced() {
        let mut t = empty();
        for key in ["m", "c", "p", "a", "e", "n", "z", "b", "d"] {
            t = insert(&t, k(key), k(key));
        }
        // 리프, 자식 하나, 자식 둘을 두루 지운다.
        for key in ["a", "m", "z"] {
            t = remove(&t, key.as_bytes());
            assert_eq!(get(&t, key.as_bytes()), None);
            assert_balanced(&t);
        }
        // 나머지는 전부 살아있어야 한다.
        for key in ["c", "p", "e", "n", "b", "d"] {
            assert_eq!(get(&t, key.as_bytes()), Some(key.as_bytes()));
        }
    }

    #[test]
    fn diff_reports_added_removed_modified() {
        // a: {a=1, b=2, c=3}
        let a = insert(&insert(&insert(&empty(), k("a"), k("1")), k("b"), k("2")), k("c"), k("3"));
        // b: {a=1, b=9, d=4}  (b 수정, c 삭제, d 추가)
        let b = insert(&insert(&insert(&empty(), k("a"), k("1")), k("b"), k("9")), k("d"), k("4"));

        let changes = diff(&a, &b);
        assert_eq!(
            changes,
            vec![
                Change::Modified { key: k("b"), old: k("2"), new: k("9") },
                Change::Removed { key: k("c") },
                Change::Added { key: k("d"), value: k("4") },
            ]
        );
    }

    #[test]
    fn diff_of_identical_trees_is_empty() {
        let a = insert(&insert(&empty(), k("x"), k("1")), k("y"), k("2"));
        let b = insert(&insert(&empty(), k("x"), k("1")), k("y"), k("2"));
        assert!(diff(&a, &b).is_empty());
    }

    #[test]
    fn structural_sharing_is_real() {
        // 삽입 후에도 건드리지 않은 부분 트리가 복사가 아니라 *공유*됨을 증명한다.
        // 트리를 만든 뒤, 루트의 오른쪽으로 가는 키를 삽입한다. 그러면 루트의 왼쪽
        // 부분 트리는 두 버전에서 똑같은 Arc여야 한다(회전이 일어나도 왼쪽은 안 건드림).
        let base = insert(&insert(&insert(&empty(), k("m"), k("M")), k("c"), k("C")), k("a"), k("A"));

        let root = base.as_ref().unwrap();
        let left_before = root.left.as_ref().unwrap();
        let count_before = Arc::strong_count(left_before);

        // 루트보다 큰 키 -> 오른쪽으로 간다. 왼쪽 부분 트리는 절대 건드리지 않는다.
        let after = insert(&base, k("z"), k("Z"));

        let new_root = after.as_ref().unwrap();
        let left_after = new_root.left.as_ref().unwrap();

        // 같은 할당 -> 공유 성공(왼쪽 부분 트리 복사 0).
        assert!(
            Arc::ptr_eq(left_before, left_after),
            "왼쪽 부분 트리는 복사가 아니라 버전 간에 공유되어야 한다"
        );
        assert!(Arc::strong_count(left_after) > count_before);
    }
}
