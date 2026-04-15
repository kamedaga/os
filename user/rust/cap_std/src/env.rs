extern crate alloc;

use alloc::string::String;
use core::slice;
use core::str;

use crate::path::PathBuf;

const PROCESS_ARGS_ENV_TARGET_VA: usize = 0x3C02_4000;
const PROCESS_ARGS_ENV_MAGIC: u64 = 0x5052_4147_4556_3131;
const PROCESS_ARGS_ENV_VERSION: u64 = 1;
const PROCESS_ARGS_ENV_MAX_ARGS: usize = 32;
const PROCESS_ARGS_ENV_MAX_ENV: usize = 32;
const PROCESS_ARGS_ENV_DATA_BYTES: usize = 3792;

#[repr(C)]
#[derive(Copy, Clone)]
struct Entry {
    offset: u16,
    len: u16,
}

#[repr(C)]
struct Page {
    magic: u64,
    version: u64,
    arg_count: u64,
    env_count: u64,
    string_bytes: u64,
    reserved0: u64,
    args: [Entry; PROCESS_ARGS_ENV_MAX_ARGS],
    env: [Entry; PROCESS_ARGS_ENV_MAX_ENV],
    data: [u8; PROCESS_ARGS_ENV_DATA_BYTES],
}

#[derive(Clone, Debug, Default)]
pub struct Args {
    index: usize,
}

impl Iterator for Args {
    type Item = String;

    fn next(&mut self) -> Option<Self::Item> {
        let value = arg_at(self.index)?;
        self.index += 1;
        Some(String::from(value))
    }
}

#[derive(Clone, Debug, Default)]
pub struct Vars {
    index: usize,
}

impl Iterator for Vars {
    type Item = (String, String);

    fn next(&mut self) -> Option<Self::Item> {
        let value = env_raw_at(self.index)?;
        self.index += 1;
        let split = value.find('=')?;
        Some((
            String::from(&value[..split]),
            String::from(&value[split + 1..]),
        ))
    }
}

fn validated_page() -> Option<&'static Page> {
    // SAFETY: All spawned user processes now receive an args/env bootstrap page
    // at this fixed VA, either zeroed or explicitly populated at spawn time.
    let page = unsafe { &*(PROCESS_ARGS_ENV_TARGET_VA as *const Page) };
    if page.magic != PROCESS_ARGS_ENV_MAGIC || page.version != PROCESS_ARGS_ENV_VERSION {
        return None;
    }
    if page.arg_count as usize > PROCESS_ARGS_ENV_MAX_ARGS
        || page.env_count as usize > PROCESS_ARGS_ENV_MAX_ENV
        || page.string_bytes as usize > PROCESS_ARGS_ENV_DATA_BYTES
    {
        return None;
    }
    Some(page)
}

fn entry_as_str(page: &'static Page, entry: Entry) -> Option<&'static str> {
    let offset = entry.offset as usize;
    let len = entry.len as usize;
    let end = offset.checked_add(len)?;
    if end > page.string_bytes as usize {
        return None;
    }
    let base = page.data.as_ptr();
    // SAFETY: `offset..end` stays within the validated `string_bytes` range, and
    // the page is mapped for the life of the process.
    let bytes = unsafe { slice::from_raw_parts(base.add(offset), len) };
    str::from_utf8(bytes).ok()
}

fn arg_at(index: usize) -> Option<&'static str> {
    let page = validated_page()?;
    if index >= page.arg_count as usize {
        return None;
    }
    entry_as_str(page, page.args[index])
}

fn env_raw_at(index: usize) -> Option<&'static str> {
    let page = validated_page()?;
    if index >= page.env_count as usize {
        return None;
    }
    entry_as_str(page, page.env[index])
}

pub fn args() -> Args {
    Args::default()
}

pub fn args_count() -> usize {
    validated_page().map_or(0, |page| page.arg_count as usize)
}

pub fn arg(index: usize) -> Option<String> {
    Some(String::from(arg_at(index)?))
}

pub fn vars() -> Vars {
    Vars::default()
}

pub fn vars_count() -> usize {
    validated_page().map_or(0, |page| page.env_count as usize)
}

pub fn var(key: &str) -> Option<String> {
    let mut vars = vars();
    while let Some((name, value)) = vars.next() {
        if name == key {
            return Some(value);
        }
    }
    None
}

pub fn program_name() -> Option<String> {
    arg(0)
}

pub fn current_exe() -> Option<PathBuf> {
    let arg0 = arg_at(0)?;
    if !arg0.starts_with('/') {
        return None;
    }
    Some(PathBuf::from(arg0))
}

pub fn current_dir() -> Option<PathBuf> {
    let pwd = var("PWD")?;
    Some(PathBuf::from(pwd))
}
