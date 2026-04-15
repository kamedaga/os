use core::time::Duration;

use crate::{Error, ErrorKind, Result};

#[derive(Copy, Clone, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct Instant {
    ticks: u64,
}

impl Instant {
    pub fn now() -> Self {
        Self {
            ticks: rt_io::MonotonicClock::new().now_ticks(),
        }
    }

    pub const fn from_ticks(ticks: u64) -> Self {
        Self { ticks }
    }

    pub const fn as_ticks(self) -> u64 {
        self.ticks
    }

    pub fn ticks_since(self, earlier: Self) -> u64 {
        self.ticks.saturating_sub(earlier.ticks)
    }

    pub fn elapsed_ticks(self) -> u64 {
        Self::now().ticks_since(self)
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct SystemTime {
    unix_seconds: u64,
}

impl SystemTime {
    pub fn now() -> Result<Self> {
        Err(Error::new(ErrorKind::Unsupported))
    }

    pub const fn from_unix_seconds(unix_seconds: u64) -> Self {
        Self { unix_seconds }
    }

    pub const fn as_unix_seconds(self) -> u64 {
        self.unix_seconds
    }

    pub fn duration_since(self, earlier: Self) -> Duration {
        Duration::from_secs(self.unix_seconds.saturating_sub(earlier.unix_seconds))
    }
}
