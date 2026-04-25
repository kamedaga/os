#![no_std]
#![no_main]

use rt_alloc as _;

fn main() -> cap_std::Result<()> {
    cap_std::println!("helloworld")?;
    Ok(())
}

cap_std::entry_point!(main);
