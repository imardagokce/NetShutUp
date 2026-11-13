# NetShutUp Modern

Modern is being developed as a Rust application for Windows 10/11. This branch is the Modern product line; the Legacy product line is maintained separately on the `legacy` branch.

## First milestone: minimal read-only prototype

The initial executable only reports the operating-system family and architecture provided by Rust's standard library. It does not inspect adapters, run system commands, request elevation, access the network, or change settings.

This intentionally small first step gives us a buildable project before adding Windows-specific diagnostics. No compatibility claim is made until the application is tested on the target Windows versions.

## Build and run

Install the stable Rust toolchain, then from this directory run:

```sh
cargo run
```

To build without running:

```sh
cargo build --release
```

## Next step

Add a read-only Windows version/build diagnostic and tests. Only after that is verified should we enumerate network adapters. Profile changes and rollback are out of scope for this prototype.
