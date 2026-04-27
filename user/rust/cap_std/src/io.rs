extern crate alloc;

use alloc::vec::Vec;
use core::cmp;
use core::fmt;

use crate::Result;

const READ_TO_END_CHUNK_BYTES: usize = 3840;

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum SeekFrom {
    Start(u64),
    Current(i64),
    End(i64),
}

pub trait Read {
    /// Blocking read.
    ///
    /// Implementations return only after data, EOF, or an error is available.
    /// A short read is allowed; callers that need an exact byte count must use
    /// `read_exact`.
    fn read(&mut self, buf: &mut [u8]) -> Result<usize>;

    fn read_exact(&mut self, mut buf: &mut [u8]) -> Result<()> {
        while !buf.is_empty() {
            let read = self.read(buf)?;
            if read == 0 {
                return Err(crate::Error::new(crate::ErrorKind::Other));
            }
            buf = &mut buf[read..];
        }
        Ok(())
    }

    fn read_to_end(&mut self, out: &mut Vec<u8>) -> Result<usize> {
        let mut total = 0usize;
        let mut chunk = [0u8; READ_TO_END_CHUNK_BYTES];
        loop {
            let read = self.read(&mut chunk)?;
            if read == 0 {
                break;
            }
            out.extend_from_slice(&chunk[..read]);
            total += read;
        }
        Ok(total)
    }

    fn read_to_end_bounded(&mut self, out: &mut Vec<u8>, max_bytes: usize) -> Result<usize> {
        let mut total = 0usize;
        let mut chunk = [0u8; READ_TO_END_CHUNK_BYTES];
        loop {
            let remaining = max_bytes
                .checked_sub(total)
                .ok_or_else(|| crate::Error::new(crate::ErrorKind::BufferTooSmall))?;
            if remaining == 0 {
                let mut probe = [0u8; 1];
                if self.read(&mut probe)? == 0 {
                    return Ok(total);
                }
                return Err(crate::Error::new(crate::ErrorKind::BufferTooSmall));
            }

            let read_len = cmp::min(chunk.len(), remaining);
            let read = self.read(&mut chunk[..read_len])?;
            if read == 0 {
                break;
            }
            out.extend_from_slice(&chunk[..read]);
            total += read;
        }
        Ok(total)
    }
}

pub trait Write {
    /// Blocking write.
    ///
    /// Implementations return after accepting at least part of the buffer or
    /// reporting an error. A short write is allowed; callers that need the
    /// whole buffer committed must use `write_all`.
    fn write(&mut self, buf: &[u8]) -> Result<usize>;

    fn flush(&mut self) -> Result<()> {
        Ok(())
    }

    fn write_all(&mut self, mut buf: &[u8]) -> Result<()> {
        while !buf.is_empty() {
            let written = self.write(buf)?;
            if written == 0 {
                return Err(crate::Error::new(crate::ErrorKind::Other));
            }
            buf = &buf[written..];
        }
        Ok(())
    }

    fn write_fmt(&mut self, args: fmt::Arguments<'_>) -> Result<()>
    where
        Self: Sized,
    {
        struct Adapter<'a, W: ?Sized> {
            writer: &'a mut W,
            error: Option<crate::Error>,
        }

        impl<W> fmt::Write for Adapter<'_, W>
        where
            W: Write + ?Sized,
        {
            fn write_str(&mut self, s: &str) -> fmt::Result {
                match self.writer.write_all(s.as_bytes()) {
                    Ok(()) => Ok(()),
                    Err(err) => {
                        self.error = Some(err);
                        Err(fmt::Error)
                    }
                }
            }
        }

        let mut adapter = Adapter {
            writer: self,
            error: None,
        };
        match fmt::write(&mut adapter, args) {
            Ok(()) => Ok(()),
            Err(_) => Err(adapter
                .error
                .unwrap_or_else(|| crate::Error::new(crate::ErrorKind::Other))),
        }
    }
}

pub trait Seek {
    fn seek(&mut self, position: SeekFrom) -> Result<u64>;
}

#[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
pub struct LogWriter;

pub const fn log() -> LogWriter {
    LogWriter
}

pub fn copy<R, W>(reader: &mut R, writer: &mut W) -> Result<u64>
where
    R: Read + ?Sized,
    W: Write + ?Sized,
{
    let mut copied = 0u64;
    let mut buf = [0u8; 256];
    loop {
        let read = reader.read(&mut buf)?;
        if read == 0 {
            return Ok(copied);
        }
        writer.write_all(&buf[..read])?;
        copied = copied
            .checked_add(read as u64)
            .ok_or_else(|| crate::Error::new(crate::ErrorKind::Other))?;
    }
}

impl Write for LogWriter {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        rt_core::log_bytes(buf);
        Ok(buf.len())
    }
}

impl fmt::Write for LogWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.write_all(s.as_bytes()).map_err(|_| fmt::Error)
    }
}

pub(crate) fn apply_seek(current: u64, end: u64, position: SeekFrom) -> Result<u64> {
    let next = match position {
        SeekFrom::Start(offset) => Some(offset),
        SeekFrom::Current(delta) => {
            if delta >= 0 {
                current.checked_add(delta as u64)
            } else {
                current.checked_sub(delta.unsigned_abs())
            }
        }
        SeekFrom::End(delta) => {
            if delta >= 0 {
                end.checked_add(delta as u64)
            } else {
                end.checked_sub(delta.unsigned_abs())
            }
        }
    };
    next.map(|value| cmp::min(value, end))
        .ok_or_else(|| crate::Error::new(crate::ErrorKind::InvalidInput))
}
