//! branchdb의 파이썬 바인딩.
//!
//! 코어 `branchdb` 크레이트(의존성 0)를 그대로 감싸, 에이전트 개발자가 익숙한
//! 파이썬에서 바로 쓸 수 있게 한다. 키와 값은 파이썬 `bytes`로 주고받는다.
//!
//! 빌드(개발 설치):
//! ```sh
//! cd bindings/python
//! pip install maturin
//! maturin develop
//! ```
//! 그러면 파이썬에서 `import branchdb` 가 된다.

use pyo3::exceptions::PyValueError;
use pyo3::prelude::*;
use pyo3::types::PyBytes;

use ::branchdb::tree::Change;
use ::branchdb::BranchDB as CoreDB;
use ::branchdb::Error as CoreError;

fn to_py_err(err: CoreError) -> PyErr {
    PyValueError::new_err(err.to_string())
}

fn io_to_py_err(err: std::io::Error) -> PyErr {
    PyValueError::new_err(err.to_string())
}

/// AI 에이전트를 위한 임베디드 · 브랜치 가능한 키-값 저장소.
#[pyclass(name = "BranchDB")]
struct PyBranchDB {
    inner: CoreDB,
}

#[pymethods]
impl PyBranchDB {
    /// 빈 `main` 브랜치 하나를 가진 새 DB를 만든다.
    #[new]
    fn new() -> Self {
        PyBranchDB { inner: CoreDB::new() }
    }

    /// 파일에서 DB를 읽어 복원한다.
    #[staticmethod]
    fn open(path: &str) -> PyResult<Self> {
        Ok(PyBranchDB {
            inner: CoreDB::open(path).map_err(io_to_py_err)?,
        })
    }

    /// DB 전체를 한 파일로 저장한다.
    fn save(&self, path: &str) -> PyResult<()> {
        self.inner.save(path).map_err(io_to_py_err)
    }

    /// 현재 존재하는 브랜치 이름들(정렬됨).
    fn branch_names(&self) -> Vec<String> {
        self.inner.branch_names().into_iter().map(str::to_string).collect()
    }

    /// 비어 있는 새 브랜치를 만든다.
    fn create_branch(&mut self, name: &str) -> PyResult<()> {
        self.inner.create_branch(name).map_err(to_py_err)
    }

    /// `from` 브랜치를 통째로 `to`로 포크한다(O(1)).
    fn fork(&mut self, from: &str, to: &str) -> PyResult<()> {
        self.inner.fork(from, to).map_err(to_py_err)
    }

    /// 한 브랜치에 키-값을 넣는다.
    fn put(&mut self, branch: &str, key: &[u8], value: &[u8]) -> PyResult<()> {
        self.inner.put(branch, key.to_vec(), value.to_vec()).map_err(to_py_err)
    }

    /// 한 브랜치에서 키를 조회한다. 없으면 `None`.
    fn get<'py>(
        &self,
        py: Python<'py>,
        branch: &str,
        key: &[u8],
    ) -> PyResult<Option<Bound<'py, PyBytes>>> {
        let value = self.inner.get(branch, key).map_err(to_py_err)?;
        Ok(value.map(|bytes| PyBytes::new(py, bytes)))
    }

    /// 한 브랜치에서 키를 삭제한다.
    fn delete(&mut self, branch: &str, key: &[u8]) -> PyResult<()> {
        self.inner.delete(branch, key).map_err(to_py_err)
    }

    /// `from`이 갈라진 뒤 바꾼 것만 `into`에 적용한다(3-way merge, 삭제 포함).
    fn merge(&mut self, from: &str, into: &str) -> PyResult<()> {
        self.inner.merge(from, into).map_err(to_py_err)
    }

    /// 두 브랜치의 차이를 `(op, key, value)` 튜플 목록으로 돌려준다.
    /// `op`은 "added" / "removed" / "modified", value는 removed일 때 `None`.
    fn diff(
        &self,
        py: Python<'_>,
        a: &str,
        b: &str,
    ) -> PyResult<Vec<(String, Py<PyBytes>, Option<Py<PyBytes>>)>> {
        let changes = self.inner.diff(a, b).map_err(to_py_err)?;
        let mut out = Vec::with_capacity(changes.len());
        for change in changes {
            let row = match change {
                Change::Added { key, value } => (
                    "added".to_string(),
                    PyBytes::new(py, &key).unbind(),
                    Some(PyBytes::new(py, &value).unbind()),
                ),
                Change::Removed { key } => {
                    ("removed".to_string(), PyBytes::new(py, &key).unbind(), None)
                }
                Change::Modified { key, new, .. } => (
                    "modified".to_string(),
                    PyBytes::new(py, &key).unbind(),
                    Some(PyBytes::new(py, &new).unbind()),
                ),
            };
            out.push(row);
        }
        Ok(out)
    }
}

#[pymodule]
fn branchdb(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<PyBranchDB>()?;
    Ok(())
}
