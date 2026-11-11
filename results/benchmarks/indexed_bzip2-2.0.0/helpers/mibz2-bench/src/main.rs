//! Benchmark driver for mibz2's streaming decoder.
//!
//! The PyPI package lags the crates.io release (0.2.6 versus 0.3.1 at the time of writing), so this
//! driver uses the audited 0.3.1 crate directly. It reads and discards chunks like the other
//! streaming decoders, and checks the stream CRC by reading to EOF.

use std::hint::black_box;
use std::io::Read;

fn usage() -> ! {
    eprintln!("usage: mibz2 <threads> <input.bz2>");
    std::process::exit(2);
}

fn main() -> std::io::Result<()> {
    let mut arguments = std::env::args().skip(1);
    let threads = arguments
        .next()
        .unwrap_or_else(|| usage())
        .parse::<usize>()
        .unwrap_or_else(|error| {
            eprintln!("invalid thread count: {error}");
            std::process::exit(2);
        });
    let path = arguments.next().unwrap_or_else(|| usage());
    if arguments.next().is_some() {
        usage();
    }

    let file = std::fs::File::open(path)?;
    let mut reader = if threads == 1 {
        mibz2::BzReader::new(file)
    } else {
        mibz2::BzReader::parallel(file, threads)
    };
    let mut buffer = vec![0u8; 1 << 16];
    loop {
        let read = reader.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        black_box(read);
    }
    Ok(())
}
