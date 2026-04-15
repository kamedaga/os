extern crate alloc;

use alloc::string::String;
use core::ops::Deref;

#[repr(transparent)]
#[derive(Debug, Eq, PartialEq)]
pub struct Path(str);

impl Path {
    pub fn new(path: &str) -> &Self {
        // SAFETY: `Path` is a transparent wrapper over `str`.
        unsafe { &*(path as *const str as *const Self) }
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }

    pub fn is_absolute(&self) -> bool {
        self.as_str().starts_with('/')
    }

    pub fn is_empty(&self) -> bool {
        self.as_str().is_empty()
    }
}

impl AsRef<Path> for Path {
    fn as_ref(&self) -> &Path {
        self
    }
}

impl AsRef<Path> for str {
    fn as_ref(&self) -> &Path {
        Path::new(self)
    }
}

impl AsRef<Path> for String {
    fn as_ref(&self) -> &Path {
        Path::new(self.as_str())
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct PathBuf {
    inner: String,
}

impl PathBuf {
    pub const fn new() -> Self {
        Self {
            inner: String::new(),
        }
    }

    pub fn from_path<P>(path: P) -> Self
    where
        P: AsRef<Path>,
    {
        Self {
            inner: String::from(path.as_ref().as_str()),
        }
    }

    pub fn as_path(&self) -> &Path {
        Path::new(self.inner.as_str())
    }

    pub fn as_str(&self) -> &str {
        self.inner.as_str()
    }

    pub fn into_string(self) -> String {
        self.inner
    }

    pub fn push<P>(&mut self, path: P)
    where
        P: AsRef<Path>,
    {
        let part = path.as_ref().as_str();
        if part.is_empty() {
            return;
        }
        if self.inner.is_empty() {
            self.inner.push_str(part);
            return;
        }
        let needs_separator = !self.inner.ends_with('/') && !part.starts_with('/');
        if needs_separator {
            self.inner.push('/');
        }
        if self.inner.ends_with('/') && part.starts_with('/') {
            self.inner.push_str(&part[1..]);
        } else {
            self.inner.push_str(part);
        }
    }
}

impl AsRef<Path> for PathBuf {
    fn as_ref(&self) -> &Path {
        self.as_path()
    }
}

impl Deref for PathBuf {
    type Target = Path;

    fn deref(&self) -> &Self::Target {
        self.as_path()
    }
}

impl From<&str> for PathBuf {
    fn from(value: &str) -> Self {
        Self::from_path(value)
    }
}

impl From<String> for PathBuf {
    fn from(value: String) -> Self {
        Self { inner: value }
    }
}
