//! Benchmark driver for crabz2's parallel whole-buffer decoder.
//!
//! crabz2 exposes parallelism only through `decompress_parallel`, which returns the complete
//! plaintext. The other benchmark tools stream to `/dev/null`, so this driver reads the input,
//! decodes it with the requested thread count, and only keeps the output length alive.

use std::hint::black_box;

fn usage() -> ! {
    eprintln!("usage: crabz2 <threads> <input.bz2>");
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

    let compressed = std::fs::read(path)?;
    let decompressed = crabz2::decompress_parallel(&compressed, Some(threads))?;
    black_box(decompressed.len());
    Ok(())
}
