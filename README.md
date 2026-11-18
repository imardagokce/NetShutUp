<div align="center">

# NetShutUp

**Pick a profile. See exactly what changes. Apply it, and undo it with one command.**

A single-file Windows tool that tunes network privacy and performance settings, safely.

[![License: GPL v3](https://img.shields.io/badge/license-GPL%20v3-blue.svg?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-0078D6?style=flat-square&logo=windows&logoColor=white)](#requirements)
[![Rust](https://img.shields.io/badge/rust-2024%20edition-DEA584?style=flat-square&logo=rust&logoColor=white)](modern/Cargo.toml)
[![Status](https://img.shields.io/badge/status-pre--alpha-orange?style=flat-square)](#status)
[![Dependabot](https://img.shields.io/badge/dependabot-enabled-025E8C?style=flat-square&logo=dependabot&logoColor=white)](.github/dependabot.yml)
[![Last commit](https://img.shields.io/github/last-commit/imardagokce/NetShutUp?style=flat-square)](https://github.com/imardagokce/NetShutUp/commits/main)
[![Issues](https://img.shields.io/github/issues/imardagokce/NetShutUp?style=flat-square)](https://github.com/imardagokce/NetShutUp/issues)

[Features](#features) · [Profiles](#profiles) · [Quick start](#quick-start) · [Safety](#safety-first) · [Help us test](#help-us-test) · [Roadmap](#roadmap) · [Contributing](#contributing)

</div>

---

## What is it?

NetShutUp is the network-focused cousin of tools like O&O ShutUp10. You choose a profile — **Privacy**, **Performance** or **Recommended** — and the program shows you every registry value, service and DNS setting it would touch, asks for confirmation, saves a backup, applies the change and checks the result.

It exists because "network optimization" tips on the internet are a mixed bag: some help, some do nothing, some quietly make things worse. NetShutUp only includes tweaks whose registry path, default value and effect were checked against Microsoft's documentation, and it leaves out the folklore (for example the "disable auto-tuning" and QoS-bandwidth tricks).

> [!WARNING]
> **Pre-alpha.** The code that changes settings has been tested with unit tests and a registry sandbox, but it has not yet been proven on a range of real computers. Start with `preview` and `--dry-run`, and read [Help us test](#help-us-test) if you want to try it for real.

## Features

- **Read-only first.** `scan` and `preview` show the current state and the planned changes without touching anything and without administrator rights.
- **Backup before every change.** The previous value of every touched setting is saved and verified on disk before the first write. A setting that was *not set* is restored as *not set*, not as zero.
- **One-command undo.** `revert` puts everything back from the backup; a failed write rolls back everything done so far.
- **Encrypted DNS that proves itself.** The profile points your adapters at a DNS provider with DNS-over-HTTPS required, then checks with real lookups that it works. If it does not work on your network, the next provider is tried, and if nothing works your old DNS is restored. You are never left without internet.
- **Interactive menu.** Double-click the `.exe` for an arrow-key menu; or use plain commands in a terminal or script.
- **Self-test mode.** One menu entry runs every profile for real, undoes it, verifies that the computer is back exactly as it was, and writes a masked report you can attach to an issue.
- **One file.** No installer, no extra DLLs, no background service, nothing sent over the network.

## Profiles

| Profile | For | What it does |
|---|---|---|
| **Privacy** | Less telemetry, fewer leaks | Disables the telemetry service, sets the diagnostic-data policy to the lowest level, turns off LLMNR name broadcasts and peer-to-peer update sharing, and moves DNS to **Quad9** with DoH. |
| **Performance** | Games and real-time apps | Turns off delayed ACK per adapter, removes the multimedia network-throttling limit, turns off peer-to-peer update sharing, and picks the faster of **Cloudflare** and **Google** DNS (DoH) by measuring. |
| **Recommended** | Everyday use | Turns off LLMNR and peer-to-peer update sharing and moves DNS to **Cloudflare** (then Google) with DoH. Safe, widely useful, nothing that gets in the way. |

> The Home edition of Windows treats telemetry level 0 as the lowest *allowed* level, not "off". NetShutUp says so instead of claiming otherwise. Details and the tweaks that were deliberately left out are in [`docs/design/2026-11-16-alpha-release.md`](docs/design/2026-11-16-alpha-release.md).

## Quick start

There is no release build yet; build from source (a few minutes, Rust required):

```sh
git clone https://github.com/imardagokce/NetShutUp.git
cd NetShutUp/modern
cargo build --release
```

Then run `target\release\netshutup-modern.exe`, or from a terminal:

```text
netshutup-modern                       Open the menu
netshutup-modern scan                  Show the current network settings
netshutup-modern preview <profile>     Show what a profile would change
netshutup-modern apply <profile>       Apply a profile (asks first, saves a backup)
netshutup-modern apply <profile> --dry-run
                                       Go through apply without changing anything
netshutup-modern revert                Undo the most recent apply
netshutup-modern selftest              Test every profile for real and write a report
```

`apply`, `revert` and `selftest` need administrator rights; the program asks for them through the normal Windows prompt, and only when you run one of those. Full usage notes: [`modern/README.md`](modern/README.md).

### Requirements

| | |
|---|---|
| Windows 10 / 11 | Everything except encrypted DNS works on both. Encrypted DNS needs **Windows 11** (build 22000+); on Windows 10 the DNS step is skipped and your DNS is not touched. |
| Windows 7 / 8 / 8.1 | Not supported by this line; see the separate [`legacy`](https://github.com/imardagokce/NetShutUp/tree/legacy) branch. |

## Safety first

The rules the code is built around (and tests enforce):

1. Nothing changes before you have seen a preview and confirmed it.
2. A verified backup exists before the first change.
3. A failure halfway undoes the changes made so far.
4. After a profile that changes DNS, internet access is checked; if it does not work, DNS is put back as it was.
5. Settings are only written for physical Ethernet and Wi-Fi adapters; virtual adapters, VPNs and tunnels are left alone.
6. Nothing leaves your computer.
7. The self-test exports the relevant registry keys and creates a System Restore point before it changes anything, and writes any problem into its report.

## Help us test

Real-world testing is the most valuable thing right now.

1. Build the program (see above) or ask for a test build.
2. Run it, choose **Self-test (for testers)** in the menu, confirm, and accept the administrator prompt. It applies each profile, checks it, undoes it, and writes a report.
3. Read the report (see [CONTRIBUTING.md](CONTRIBUTING.md#testing-guide) for the full guide; it contains no user name, computer name, Wi-Fi name or full IP/MAC addresses), then [open a test report](https://github.com/imardagokce/NetShutUp/issues/new?template=test-report.yml) and attach both files.

<details>
<summary>What the self-test report contains</summary>

Windows version and edition, processor and memory, computer make and model, network adapter hardware and drivers, how many VPN or virtual adapters exist, whether the computer is on a domain or has DNS/telemetry policies, the names of registered antivirus/firewall products, whether a proxy is set, and what every test did and how long it took. Addresses are masked; nothing is uploaded.

</details>

Found a security problem? Please use the private route described in [`SECURITY.md`](SECURITY.md) instead of a public issue.

## Roadmap

- [x] Windows version detection, adapter enumeration, settings catalog and read-only scan
- [x] Backup, apply and revert engine with rollback
- [x] Three built-in profiles with preview and dry-run
- [x] Interactive terminal menu
- [x] DNS provider with encrypted DNS (DoH), real-lookup trial and automatic fallback
- [x] Self-test mode with a masked report
- [x] DNS-over-TLS as a second encrypted DNS method (experimental, tried when DoH does not work)
- [ ] Windows Firewall baseline for all profiles
- [ ] First alpha release (`0.1.0-alpha.1`) with a prebuilt `.exe`
- [ ] Advanced mode: switch individual settings on and off
- [ ] Graphical interface

## Repository layout

```text
modern/    Rust application for Windows 10/11 (this branch, main)
docs/      Design notes, roadmap and compatibility matrix
legacy     Separate branch for Windows 7/8/8.1 (independent codebase)
```

## Contributing

Testers and contributors are welcome. Start with the **[Contributing & testing guide](CONTRIBUTING.md)**: it explains step by step how to test safely, how to send a report, and how to work on the code (small, tested changes; [conventional commits](https://www.conventionalcommits.org/)). Dependency security alerts are watched by Dependabot.

## License

NetShutUp is free software under the **GNU General Public License v3.0**. Forks must remain open source. See [`LICENSE`](LICENSE).
