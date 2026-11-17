# NetShutUp Modern

The Modern product line: a single-`.exe` Rust application for Windows 10/11. The Legacy product line is maintained separately on the `legacy` branch.

> **Pre-alpha.** The code that changes settings has only been exercised by unit tests and a registry sandbox so far, not yet on real systems. Use `--dry-run` first.

## What it does today

- Opens an arrow-key menu when run without arguments (double-click the `.exe`): scan, apply a profile, revert, exit.
- Reads the real Windows version and lists network adapters.
- Shows the current value of every setting it knows about and whether it differs from the Windows default. No administrator rights needed.
- Previews what a profile would change, without changing anything.
- Applies a profile after an explicit confirmation, saving a backup first.
- Reverts the most recent apply from that backup.
- Points every Ethernet and Wi-Fi adapter at a DNS provider (Quad9, Cloudflare or Google, depending on the profile) with encrypted DNS (DoH) required, checks with real lookups that it works, tries the next provider if not, and puts your old DNS back if nothing works. Needs Windows 11.

## Usage

```
netshutup-modern                    Open the menu
netshutup-modern scan               Show the current network settings
netshutup-modern preview <profile>  Show what a profile would change
netshutup-modern apply <profile>    Apply a profile (asks first, saves a backup)
netshutup-modern apply <profile> --dry-run
                                    Go through apply without changing anything
netshutup-modern revert             Undo the most recent apply
```

Profiles: `privacy`, `performance`, `recommended`.

`apply` and `revert` need administrator rights; the program asks for them through UAC only when one of those commands is run. From the menu, UAC is asked once and the menu continues in the administrator window.

Without arguments in a non-interactive context (a script, a redirect) the program prints the usage text instead of opening the menu.

## Safety

- Nothing is changed without a preview and a confirmation.
- A backup is written to `%ProgramData%\NetShutUp\backups` and verified before the first change. If a write fails, everything done so far is rolled back.
- A setting that was not set before is removed again on revert, not written as zero.
- Per-adapter settings only go to physical Ethernet and Wi-Fi adapters. Virtual adapters (Wi-Fi Direct, WAN miniports, tunnels) are left alone.
- TCP global settings (auto-tuning, ECN, RSS) are shown but never changed.
- DNS is only changed when the internet works beforehand, and is put back exactly as it was (DHCP stays DHCP) if encrypted DNS does not work on your network. IPv6 DNS servers are not touched.

## Build and test

Install the stable Rust toolchain (MSVC target), then from this directory:

```sh
cargo test
cargo run -- preview recommended
cargo build --release
```

The C runtime is linked statically, so the release `.exe` runs without extra DLLs. No test writes to real settings.

## Not there yet

Planned for `0.1.0-alpha.1`, see [`docs/design/2026-11-16-alpha-release.md`](../docs/design/2026-11-16-alpha-release.md):

- DNS over TLS (DoT) as a second encrypted DNS method, tried when DoH does not work
- Windows Firewall baseline

Later: per-setting control (Advanced Mode) and a GUI.
