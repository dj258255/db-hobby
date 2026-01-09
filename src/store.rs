//! 디스크 저장/복원 — 오프셋 주소 기반 노드 저장소.
//!
//! 핵심 아이디어는 메모리의 구조 공유를 디스크로 그대로 옮기는 것이다. 노드를
//! 파일에 한 번만 쓰고, 자식은 그 노드의 **파일 오프셋**으로 가리킨다. 저장할 때
//! `Arc` 포인터로 이미 기록한 노드를 알아채고 재사용하므로, 여러 브랜치가 공유하는
//! 노드는 디스크에도 단 한 번만 저장된다. 불러올 때도 오프셋을 캐시해 같은 노드를
//! 같은 `Arc`로 복원하므로, 저장 전의 공유 구조가 메모리에 그대로 되살아난다.
//!
//! 파일 레이아웃:
//! ```text
//! [magic "BRDB"][version u8]
//! ... 노드 레코드들 (자식이 먼저, 부모가 나중 — 후위 순서라 전방 참조가 없다) ...
//! [브랜치 테이블: u32 count, 그다음 (이름, 루트 ref) 반복]
//! [footer: u64 = 브랜치 테이블의 오프셋]
//! ```
//! 노드 레코드: `[key][value][left ref][right ref]`,
//! 길이 접두 바이트열은 `[u32 len][bytes]`, ref는 `[u8 tag][u64 offset?]`(tag 0=없음).

use std::collections::HashMap;
use std::fs;
use std::io;
use std::path::Path;
use std::sync::Arc;

use crate::tree::{Node, Tree};

const MAGIC: &[u8; 4] = b"BRDB";
const VERSION: u8 = 1;

// ---------- 저장 ----------

/// 모든 브랜치를 한 파일로 저장한다. 임시 파일에 쓴 뒤 rename 해서, 도중에 죽어도
/// 기존 파일이 깨지지 않게 한다.
pub fn save(branches: &HashMap<String, Tree>, path: impl AsRef<Path>) -> io::Result<()> {
    let mut buf: Vec<u8> = Vec::new();
    buf.extend_from_slice(MAGIC);
    buf.push(VERSION);

    // Arc 포인터 주소 -> 파일 오프셋. 이미 쓴 노드를 재사용하기 위한 캐시.
    let mut written: HashMap<usize, u64> = HashMap::new();

    // 브랜치 이름을 정렬해 결정적인 파일을 만든다.
    let mut names: Vec<&String> = branches.keys().collect();
    names.sort();

    let mut roots: Vec<(&str, Option<u64>)> = Vec::new();
    for name in names {
        let root = write_node(&branches[name], &mut buf, &mut written);
        roots.push((name, root));
    }

    // 브랜치 테이블.
    let table_offset = buf.len() as u64;
    write_u32(&mut buf, roots.len() as u32);
    for (name, root) in roots {
        write_bytes(&mut buf, name.as_bytes());
        write_ref(&mut buf, root);
    }
    // footer: 테이블 시작 오프셋.
    buf.extend_from_slice(&table_offset.to_le_bytes());

    let path = path.as_ref();
    let tmp = path.with_extension("tmp");
    fs::write(&tmp, &buf)?;
    fs::rename(&tmp, path)?;
    Ok(())
}

/// 한 부분 트리를 (필요하면) 기록하고 그 루트의 오프셋을 반환한다. 빈 트리는 `None`.
/// 자식을 먼저 쓰므로 부모는 항상 자기보다 앞선 오프셋만 참조한다(전방 참조 없음).
fn write_node(tree: &Tree, buf: &mut Vec<u8>, written: &mut HashMap<usize, u64>) -> Option<u64> {
    let node = tree.as_ref()?;
    let id = Arc::as_ptr(node) as usize;
    if let Some(&off) = written.get(&id) {
        return Some(off); // 이미 기록됨 -> 재사용(디스크에서도 공유).
    }
    let left = write_node(&node.left, buf, written);
    let right = write_node(&node.right, buf, written);
    let off = buf.len() as u64;
    write_bytes(buf, &node.key);
    write_bytes(buf, &node.value);
    write_ref(buf, left);
    write_ref(buf, right);
    written.insert(id, off);
    Some(off)
}

fn write_u32(buf: &mut Vec<u8>, n: u32) {
    buf.extend_from_slice(&n.to_le_bytes());
}

fn write_bytes(buf: &mut Vec<u8>, bytes: &[u8]) {
    write_u32(buf, bytes.len() as u32);
    buf.extend_from_slice(bytes);
}

fn write_ref(buf: &mut Vec<u8>, r: Option<u64>) {
    match r {
        None => buf.push(0),
        Some(off) => {
            buf.push(1);
            buf.extend_from_slice(&off.to_le_bytes());
        }
    }
}

// ---------- 복원 ----------

/// 파일에서 모든 브랜치를 읽어 복원한다.
pub fn load(path: impl AsRef<Path>) -> io::Result<HashMap<String, Tree>> {
    let data = fs::read(path)?;
    if data.len() < MAGIC.len() + 1 + 8 || &data[..4] != MAGIC {
        return Err(invalid("매직 불일치 또는 파일이 너무 짧음"));
    }
    if data[4] != VERSION {
        return Err(invalid("지원하지 않는 파일 버전"));
    }

    // footer에서 브랜치 테이블 위치를 읽는다.
    let mut pos = data.len() - 8;
    let table_offset = read_u64(&data, &mut pos)? as usize;

    let mut pos = table_offset;
    let count = read_u32(&data, &mut pos)?;
    // 오프셋 -> Arc. 같은 노드를 같은 Arc로 복원해 공유를 되살린다.
    let mut cache: HashMap<u64, Arc<Node>> = HashMap::new();
    let mut branches: HashMap<String, Tree> = HashMap::new();

    for _ in 0..count {
        let name_bytes = read_bytes(&data, &mut pos)?;
        let name = String::from_utf8(name_bytes).map_err(|_| invalid("브랜치 이름이 UTF-8이 아님"))?;
        let root_ref = read_ref(&data, &mut pos)?;
        let tree = match root_ref {
            None => None,
            Some(off) => Some(read_node(&data, off, &mut cache)?),
        };
        branches.insert(name, tree);
    }
    Ok(branches)
}

/// 한 오프셋의 노드를 읽어 `Arc<Node>`로 복원한다. 이미 읽은 오프셋이면 캐시된
/// `Arc`를 복제해 공유를 유지한다.
fn read_node(data: &[u8], offset: u64, cache: &mut HashMap<u64, Arc<Node>>) -> io::Result<Arc<Node>> {
    if let Some(node) = cache.get(&offset) {
        return Ok(node.clone());
    }
    let mut pos = offset as usize;
    let key = read_bytes(data, &mut pos)?;
    let value = read_bytes(data, &mut pos)?;
    let left_ref = read_ref(data, &mut pos)?;
    let right_ref = read_ref(data, &mut pos)?;

    let left = match left_ref {
        None => None,
        Some(off) => Some(read_node(data, off, cache)?),
    };
    let right = match right_ref {
        None => None,
        Some(off) => Some(read_node(data, off, cache)?),
    };

    // height는 디스크에 저장하지 않으므로 자식에서 다시 계산한다.
    let height = 1 + crate::tree::height(&left).max(crate::tree::height(&right));
    let node = Arc::new(Node { key, value, left, right, height });
    cache.insert(offset, node.clone());
    Ok(node)
}

fn invalid(msg: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, format!("손상된 branchdb 파일: {msg}"))
}

/// `data[pos..pos+n]`을 돌려주고 `pos`를 전진시킨다. 경계를 넘으면 오류.
fn take<'a>(data: &'a [u8], pos: &mut usize, n: usize) -> io::Result<&'a [u8]> {
    let end = pos.checked_add(n).ok_or_else(|| invalid("오프셋 오버플로"))?;
    if end > data.len() {
        return Err(invalid("예상보다 일찍 끝남(EOF)"));
    }
    let slice = &data[*pos..end];
    *pos = end;
    Ok(slice)
}

fn read_u32(data: &[u8], pos: &mut usize) -> io::Result<u32> {
    let b = take(data, pos, 4)?;
    Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

fn read_u64(data: &[u8], pos: &mut usize) -> io::Result<u64> {
    let b = take(data, pos, 8)?;
    Ok(u64::from_le_bytes(b.try_into().unwrap()))
}

fn read_bytes(data: &[u8], pos: &mut usize) -> io::Result<Vec<u8>> {
    let len = read_u32(data, pos)? as usize;
    Ok(take(data, pos, len)?.to_vec())
}

fn read_ref(data: &[u8], pos: &mut usize) -> io::Result<Option<u64>> {
    let tag = take(data, pos, 1)?[0];
    match tag {
        0 => Ok(None),
        1 => Ok(Some(read_u64(data, pos)?)),
        _ => Err(invalid("잘못된 ref 태그")),
    }
}
