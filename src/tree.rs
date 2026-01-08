//! 영속(불변) 순서 키-값 트리.
//!
//! 여기서 "영속(persistent)"은 자료구조 용어다. 모든 수정이 옛 버전을 그대로
//! 살려둔 채 *새* 버전을 반환한다는 뜻이다. 제자리에서 바뀌는 것은 아무것도
//! 없다. 바로 이 성질 덕분에 브랜치가 공짜가 된다 — 브랜치는 루트 노드를
//! 가리키는 포인터 하나일 뿐이고, 두 브랜치는 둘 다 건드리지 않은 노드를 전부
//! 공유한다.
//!
//! 명료함을 위해 평범한 이진 탐색 트리(BST)를 쓴다. 균형을 잡지 않아서 최악의
//! 경우 O(n)이다. 균형(AVL)은 다음 단계로 문서에 남겨둔다. 브랜치라는 핵심은
//! 균형과 무관하므로, 엔진을 먼저 읽기 쉽게 유지한다.

use std::cmp::Ordering;
use std::sync::Arc;

/// 키와 값은 그냥 바이트 덩어리다 — 실제 KV 저장소처럼 내용은 신경 쓰지 않는다.
pub type Key = Vec<u8>;
pub type Value = Vec<u8>;

/// 트리의 노드 하나. 불변이다: 한 번 만들어지면 필드는 절대 바뀌지 않는다.
///
/// `left`와 `right`는 `Tree`(즉 `Option<Arc<Node>>`)다. `Arc`는 원자적 참조
/// 카운팅 포인터다. 복제(clone)해도 *데이터는 복사되지 않고*, 카운터만 1 오른다.
/// 이 복제가 구조 공유의 비밀 전부다.
#[derive(Debug)]
pub struct Node {
    pub key: Key,
    pub value: Value,
    pub left: Tree,
    pub right: Tree,
}

/// 트리 전체(또는 부분 트리)는 그냥 루트 노드를 가리키는 선택적 공유 포인터다.
/// `None`이 빈 트리다. `Tree`의 복제는 O(1)이다.
pub type Tree = Option<Arc<Node>>;

impl Node {
    /// 자식이 없는 새 리프 노드를 만든다.
    fn leaf(key: Key, value: Value) -> Arc<Node> {
        Arc::new(Node { key, value, left: None, right: None })
    }

    /// `left` 자식만 다른, 이 노드의 복제본을 반환한다.
    ///
    /// 무엇이 *공유*되는지 보라: `self.right.clone()`은 오른쪽 부분 트리를
    /// 복사하지 *않는다* — `Arc` 카운트만 올린다. 그래서 새 노드와 옛 노드는
    /// 오른쪽 부분 트리 전체를 공유한다. 이것이 경로 복사(path copying)다.
    /// 루트에서 변경 지점까지의 경로 위 노드만 새로 만들고, 나머지는 전부 재사용한다.
    fn with_left(&self, left: Tree) -> Arc<Node> {
        Arc::new(Node {
            key: self.key.clone(),
            value: self.value.clone(),
            left,
            right: self.right.clone(), // <-- 공유, O(1)
        })
    }

    /// `right` 자식만 다른 복제본을 반환한다. (`with_left`의 거울상.)
    fn with_right(&self, right: Tree) -> Arc<Node> {
        Arc::new(Node {
            key: self.key.clone(),
            value: self.value.clone(),
            left: self.left.clone(), // <-- 공유, O(1)
            right,
        })
    }

    /// 값만 다른 복제본을 반환한다. (같은 키, 같은 자식.)
    fn with_value(&self, value: Value) -> Arc<Node> {
        Arc::new(Node {
            key: self.key.clone(),
            value,
            left: self.left.clone(),
            right: self.right.clone(),
        })
    }
}

/// 빈 트리.
pub fn empty() -> Tree {
    None
}

/// 키를 조회한다. 저장된 값의 참조를 반환하거나, 없으면 `None`.
///
/// 평범한 BST 하강이다. 트리를 바꾸지 않고 읽기만 한다 — 트리가 불변이라
/// 언제나 안전하다.
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

/// 키를 삽입(또는 덮어쓰기)하고 *새* 트리를 반환한다. 입력 `tree`는 전혀 손대지
/// 않고 그대로 유효하다 — 이게 핵심 전부다.
///
/// 변경 지점까지의 경로만 다시 만드는 재귀다. 모든 `with_left`/`with_right`는
/// 내려가지 않은 쪽 부분 트리를 그대로 공유한다. 그래서 루트→리프 경로 위의
/// 노드만 새로 만들어진다 = O(log n)개만 새로, 나머지는 옛 트리와 새 트리가
/// 동시에 재사용한다.
pub fn insert(tree: &Tree, key: Key, value: Value) -> Tree {
    match tree {
        // 빈 자리: 새 트리는 리프 하나다.
        None => Some(Node::leaf(key, value)),
        // 자리가 차 있음: 이 노드를 다시 만들되, 한쪽으로만 내려가고 다른 쪽은 공유.
        Some(node) => Some(match key.cmp(&node.key) {
            // 왼쪽으로: 새 왼쪽 자식은 재귀 삽입 결과, 오른쪽 부분 트리는 공유.
            Ordering::Less => node.with_left(insert(&node.left, key, value)),
            // 오른쪽으로: 거울상 — 왼쪽 부분 트리를 공유.
            Ordering::Greater => node.with_right(insert(&node.right, key, value)),
            // 같은 키가 이미 있음: 값만 바꾸고 양쪽 자식을 공유.
            Ordering::Equal => node.with_value(value),
        }),
    }
}

/// 부분 트리에서 가장 작은 (키, 값)을 찾는다. BST에서 최솟값은 맨 왼쪽 노드다.
/// `remove`의 "자식 둘" 경우에서 후속자(successor)를 찾는 데 쓴다.
fn min_kv(tree: &Tree) -> (Key, Value) {
    let mut cur = tree.as_ref().expect("빈 트리에는 최솟값이 없다");
    while let Some(left) = cur.left.as_ref() {
        cur = left;
    }
    (cur.key.clone(), cur.value.clone())
}

/// 키를 삭제하고 *새* 트리를 반환한다. `insert`와 마찬가지로 입력은 손대지 않으며,
/// 건드리지 않은 부분 트리는 그대로 공유된다.
///
/// 까다로운 경우는 "지울 노드에 자식이 둘 다 있을 때"다. 이때는 오른쪽 부분
/// 트리의 최솟값(후속자)을 끌어올려 그 자리를 채우고, 후속자를 오른쪽에서 지운다.
pub fn remove(tree: &Tree, key: &[u8]) -> Tree {
    match tree {
        None => None,
        Some(node) => match key.cmp(&node.key) {
            // 아직 못 찾음: 한쪽으로 내려가 다시 만들고, 다른 쪽은 공유.
            Ordering::Less => Some(node.with_left(remove(&node.left, key))),
            Ordering::Greater => Some(node.with_right(remove(&node.right, key))),
            // 찾음: 자식 개수에 따라 처리.
            Ordering::Equal => match (&node.left, &node.right) {
                (None, None) => None,                 // 리프: 그냥 사라진다.
                (Some(_), None) => node.left.clone(), // 자식 하나: 그 자식이 자리를 대신.
                (None, Some(_)) => node.right.clone(),
                (Some(_), Some(_)) => {
                    // 자식 둘: 오른쪽의 최솟값(후속자)을 끌어올린다.
                    let (succ_key, succ_value) = min_kv(&node.right);
                    let new_right = remove(&node.right, &succ_key);
                    Some(Arc::new(Node {
                        key: succ_key,
                        value: succ_value,
                        left: node.left.clone(), // 왼쪽은 그대로 공유.
                        right: new_right,
                    }))
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
            // a에만 있는 키 -> 삭제됨.
            Ordering::Less => {
                out.push(Change::Removed { key: ea[i].0.clone() });
                i += 1;
            }
            // b에만 있는 키 -> 추가됨.
            Ordering::Greater => {
                out.push(Change::Added { key: eb[j].0.clone(), value: eb[j].1.clone() });
                j += 1;
            }
            // 같은 키 -> 값이 다르면 수정됨.
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
    // 남은 꼬리: a 쪽은 전부 삭제, b 쪽은 전부 추가.
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
    fn remove_handles_all_child_counts() {
        // 리프, 자식 하나, 자식 둘 — 세 경우를 모두 거치도록 트리를 만든다.
        let mut t = empty();
        for key in ["m", "c", "p", "a", "e", "n", "z"] {
            t = insert(&t, k(key), k(key));
        }

        // 리프 삭제.
        let t = remove(&t, b"a");
        assert_eq!(get(&t, b"a"), None);

        // 자식 둘인 노드 삭제(루트 "m"): 나머지는 전부 살아있어야 한다.
        let t = remove(&t, b"m");
        assert_eq!(get(&t, b"m"), None);
        for key in ["c", "p", "e", "n", "z"] {
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
        //
        // 루트가 "m"이고 그 왼쪽에 "c" 아래로 부분 트리가 있는 트리를 만든다.
        // 그다음 "p"를 삽입하면(루트의 오른쪽으로 감) "c" 아래 왼쪽 부분 트리는
        // 두 버전에서 똑같은 Arc여야 한다.
        let base = insert(&insert(&insert(&empty(), k("m"), k("M")), k("c"), k("C")), k("a"), k("A"));

        // base 버전에서 루트의 왼쪽 부분 트리 Arc를 잡는다.
        let root = base.as_ref().unwrap();
        let left_before = root.left.as_ref().unwrap();
        let count_before = Arc::strong_count(left_before);

        // 루트의 오른쪽으로 가는 키를 삽입한다 — 왼쪽 부분 트리는 절대 건드리지 않음.
        let after = insert(&base, k("p"), k("P"));

        let new_root = after.as_ref().unwrap();
        let left_after = new_root.left.as_ref().unwrap();

        // 같은 할당 -> 공유 성공(왼쪽 부분 트리 복사 0).
        assert!(
            Arc::ptr_eq(left_before, left_after),
            "왼쪽 부분 트리는 복사가 아니라 버전 간에 공유되어야 한다"
        );
        // 그리고 두 번째 버전이 이제 이걸 가리키므로 참조 카운트가 올라간다.
        assert!(Arc::strong_count(left_after) > count_before);
    }
}
