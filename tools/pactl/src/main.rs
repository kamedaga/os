mod build;
mod cli;
mod config;
mod disk;
mod host_tools;
mod kernel;
mod manifest;
mod run;
mod setup;
mod sync;

use std::env;
use std::process;

fn main() {
    if let Err(err) = cli::run(env::args().skip(1).collect()) {
        eprintln!("pactl: {err}");
        process::exit(1);
    }
}
