//! `BranchDB` — 브랜치 관리자이자 라이브러리의 공개 얼굴.
//!
//! 개념은 데모에서 본 그대로다: 브랜치는 그저 **이름 -> 영속 트리의 루트** 매핑일
//! 뿐이다. 포크는 루트 포인터(`Arc`)를 복제한다 — O(1), 데이터 복사 0. 한 브랜치를
//! 수정해도 다른 브랜치는 영향받지 않는다. 트리가 불변이기 때문이다.

use std::collections::HashMap;

use crate::tree::{self, Key, Tree, Value};

/// 새 `BranchDB`가 만들어질 때 기본으로 생기는 브랜치 이름.
pub const DEFAULT_BRANCH: &str = "main";

/// `BranchDB` 연산이 실패하는 경우.
#[derive(Debug, PartialEq, Eq)]
pub enum Error {
    /// 존재하지 않는 브랜치를 가리켰다.
    NoSuchBranch(String),
    /// 이미 있는 이름으로 브랜치를 만들려 했다.
    BranchExists(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::NoSuchBranch(name) => write!(f, "그런 브랜치가 없습니다: {name}"),
            Error::BranchExists(name) => write!(f, "이미 존재하는 브랜치입니다: {name}"),
        }
    }
}

impl std::error::Error for Error {}

/// 브랜치 가능한 임베디드 키-값 저장소.
///
/// 모든 브랜치는 같은 영속 트리의 한 버전을 가리킨다. 메모리는 갈라진 만큼만
/// 늘어난다 — 공유되는 노드는 한 번만 저장된다.
pub struct BranchDB {
    branches: HashMap<String, Tree>,
}

impl BranchDB {
    /// 빈 `main` 브랜치 하나를 가진 새 DB를 만든다.
    pub fn new() -> Self {
        let mut branches = HashMap::new();
        branches.insert(DEFAULT_BRANCH.to_string(), tree::empty());
        BranchDB { branches }
    }

    /// 현재 존재하는 브랜치 이름들. (정렬해서 결정적인 순서로 돌려준다.)
    pub fn branch_names(&self) -> Vec<&str> {
        let mut names: Vec<&str> = self.branches.keys().map(String::as_str).collect();
        names.sort_unstable();
        names
    }

    /// 비어 있는 새 브랜치를 만든다.
    pub fn create_branch(&mut self, name: &str) -> Result<(), Error> {
        if self.branches.contains_key(name) {
            return Err(Error::BranchExists(name.to_string()));
        }
        self.branches.insert(name.to_string(), tree::empty());
        Ok(())
    }

    /// `from` 브랜치를 통째로 `to`라는 새 브랜치로 포크한다.
    ///
    /// **이게 이 데이터베이스의 핵심 연산이다.** 데이터 크기와 무관하게 O(1)이다 —
    /// 루트 `Arc`를 복제할 뿐, 노드는 단 하나도 복사하지 않는다. 두 브랜치는 이제
    /// 모든 노드를 공유하며, 한쪽이 수정되어 갈라지는 순간 그 경로만 새로 만들어진다.
    pub fn fork(&mut self, from: &str, to: &str) -> Result<(), Error> {
        let root = self
            .branches
            .get(from)
            .ok_or_else(|| Error::NoSuchBranch(from.to_string()))?
            .clone(); // <-- Arc 복제, O(1), 데이터 0
        if self.branches.contains_key(to) {
            return Err(Error::BranchExists(to.to_string()));
        }
        self.branches.insert(to.to_string(), root);
        Ok(())
    }

    /// 한 브랜치에 키-값을 넣는다(있으면 덮어쓴다). 그 브랜치만 새 버전으로 넘어가고,
    /// 다른 브랜치는 그대로다.
    pub fn put(&mut self, branch: &str, key: Key, value: Value) -> Result<(), Error> {
        let tree = self
            .branches
            .get(branch)
            .ok_or_else(|| Error::NoSuchBranch(branch.to_string()))?;
        let next = tree::insert(tree, key, value);
        self.branches.insert(branch.to_string(), next);
        Ok(())
    }

    /// 한 브랜치에서 키를 조회한다.
    pub fn get<'a>(&'a self, branch: &str, key: &[u8]) -> Result<Option<&'a [u8]>, Error> {
        let tree = self
            .branches
            .get(branch)
            .ok_or_else(|| Error::NoSuchBranch(branch.to_string()))?;
        Ok(tree::get(tree, key))
    }

    /// 한 브랜치에서 키를 삭제한다. 없는 키면 조용히 아무 일도 하지 않는다.
    pub fn delete(&mut self, branch: &str, key: &[u8]) -> Result<(), Error> {
        let tree = self
            .branches
            .get(branch)
            .ok_or_else(|| Error::NoSuchBranch(branch.to_string()))?;
        let next = tree::remove(tree, key);
        self.branches.insert(branch.to_string(), next);
        Ok(())
    }

    /// 브랜치를 통째로 버린다. 공유되던 노드 중 아무도 안 가리키게 된 것만
    /// 실제로 해제된다(`Arc`가 알아서 정리한다).
    pub fn drop_branch(&mut self, name: &str) -> Result<(), Error> {
        self.branches
            .remove(name)
            .map(|_| ())
            .ok_or_else(|| Error::NoSuchBranch(name.to_string()))
    }

    /// 한 브랜치의 모든 키-값을 키 순서로 돌려준다.
    pub fn entries(&self, branch: &str) -> Result<Vec<(Key, Value)>, Error> {
        let tree = self
            .branches
            .get(branch)
            .ok_or_else(|| Error::NoSuchBranch(branch.to_string()))?;
        Ok(tree::entries(tree))
    }

    /// 두 브랜치의 차이를 반환한다. `a`에서 `b`로 갈 때의 변화(추가/삭제/수정)다.
    /// 에이전트가 "이 갈래가 main과 뭐가 다른가"를 확인하는 데 쓴다.
    pub fn diff(&self, a: &str, b: &str) -> Result<Vec<tree::Change>, Error> {
        let ta = self
            .branches
            .get(a)
            .ok_or_else(|| Error::NoSuchBranch(a.to_string()))?;
        let tb = self
            .branches
            .get(b)
            .ok_or_else(|| Error::NoSuchBranch(b.to_string()))?;
        Ok(tree::diff(ta, tb))
    }

    /// `from` 브랜치의 모든 키-값을 `into` 브랜치에 적용한다(충돌 시 from이 이긴다).
    /// 에이전트가 "좋은 갈래를 main에 채택"하는 연산이다.
    ///
    /// 단순 합집합 병합이다 — `from`에서 *삭제된* 키는 전파하지 않는다. 공통 조상을
    /// 이용한 3-way 병합(삭제까지 반영)은 다음 단계로 남겨둔다.
    pub fn merge(&mut self, from: &str, into: &str) -> Result<(), Error> {
        let from_tree = self
            .branches
            .get(from)
            .ok_or_else(|| Error::NoSuchBranch(from.to_string()))?
            .clone();
        let mut into_tree = self
            .branches
            .get(into)
            .ok_or_else(|| Error::NoSuchBranch(into.to_string()))?
            .clone();
        for (key, value) in tree::entries(&from_tree) {
            into_tree = tree::insert(&into_tree, key, value);
        }
        self.branches.insert(into.to_string(), into_tree);
        Ok(())
    }
}

impl Default for BranchDB {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn b(s: &str) -> Vec<u8> {
        s.as_bytes().to_vec()
    }

    #[test]
    fn fork_is_isolated() {
        let mut db = BranchDB::new();
        db.put("main", b("user:1"), b("alice")).unwrap();

        db.fork("main", "experiment").unwrap();
        db.put("experiment", b("user:1"), b("bob")).unwrap();

        // 갈라진 브랜치만 바뀌고, main은 그대로여야 한다.
        assert_eq!(db.get("main", b"user:1").unwrap(), Some(&b"alice"[..]));
        assert_eq!(db.get("experiment", b"user:1").unwrap(), Some(&b"bob"[..]));
    }

    #[test]
    fn delete_only_affects_its_branch() {
        let mut db = BranchDB::new();
        db.put("main", b("k"), b("v")).unwrap();
        db.fork("main", "side").unwrap();

        db.delete("side", b"k").unwrap();

        assert_eq!(db.get("main", b"k").unwrap(), Some(&b"v"[..])); // main 유지
        assert_eq!(db.get("side", b"k").unwrap(), None); // side에서만 삭제
    }

    #[test]
    fn errors_are_reported() {
        let mut db = BranchDB::new();
        assert_eq!(
            db.put("ghost", b("k"), b("v")),
            Err(Error::NoSuchBranch("ghost".into()))
        );
        db.fork("main", "dup").unwrap();
        assert_eq!(db.fork("main", "dup"), Err(Error::BranchExists("dup".into())));
    }

    #[test]
    fn branch_names_are_sorted() {
        let mut db = BranchDB::new();
        db.create_branch("zeta").unwrap();
        db.create_branch("alpha").unwrap();
        assert_eq!(db.branch_names(), vec!["alpha", "main", "zeta"]);
    }

    #[test]
    fn diff_shows_what_a_branch_changed() {
        use crate::tree::Change;

        let mut db = BranchDB::new();
        db.put("main", b("keep"), b("same")).unwrap();
        db.put("main", b("edit"), b("old")).unwrap();

        db.fork("main", "exp").unwrap();
        db.put("exp", b("edit"), b("new")).unwrap(); // 수정
        db.put("exp", b("fresh"), b("v")).unwrap(); // 추가

        let changes = db.diff("main", "exp").unwrap();
        assert_eq!(
            changes,
            vec![
                Change::Modified { key: b("edit"), old: b("old"), new: b("new") },
                Change::Added { key: b("fresh"), value: b("v") },
            ]
        );
    }

    #[test]
    fn merge_adopts_the_source_branch() {
        let mut db = BranchDB::new();
        db.put("main", b("a"), b("1")).unwrap();

        db.fork("main", "feature").unwrap();
        db.put("feature", b("a"), b("2")).unwrap(); // main의 a를 덮어씀
        db.put("feature", b("b"), b("new")).unwrap(); // 새 키

        db.merge("feature", "main").unwrap();

        // main이 feature의 변경을 채택했다.
        assert_eq!(db.get("main", b"a").unwrap(), Some(&b"2"[..]));
        assert_eq!(db.get("main", b"b").unwrap(), Some(&b"new"[..]));
        // 병합 후 두 브랜치는 더 이상 차이가 없다.
        assert!(db.diff("feature", "main").unwrap().is_empty());
    }
}
