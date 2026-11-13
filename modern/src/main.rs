fn main() {
    println!("NetShutUp Modern — read-only diagnostics prototype");
    println!("Operating system: {}", std::env::consts::OS);
    println!("Architecture: {}", std::env::consts::ARCH);

    #[cfg(not(windows))]
    println!("Note: Windows-specific diagnostics are not implemented yet.");
}
