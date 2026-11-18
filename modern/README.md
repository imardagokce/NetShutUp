# NetShutUp Modern

The Modern product line: a single-`.exe` Rust application for Windows 10/11. The Legacy product line is maintained separately on the `legacy` branch.

> **Pre-alpha.** The code that changes settings has only been exercised by unit tests and a registry sandbox so far, not yet on real systems. Use `--dry-run` first.

## What it does today

- Opens an arrow-key menu when run without arguments (double-click the `.exe`): scan, apply a profile, revert, exit.
- Reads the real Windows version and lists network adapters.
- Shows the current value of every setting it knows about and whether it differs from the Windows default. No administrator rights needed.
- Makes sure Windows Firewall is on and blocks inbound connections by default for the Domain, Private and Public networks (every profile). On most computers this already holds and nothing changes. Skipped, with a note, when another firewall is registered with Windows.
- Previews what a profile would change, without changing anything.
- Applies a profile after an explicit confirmation, saving a backup first.
- Reverts the most recent apply from that backup.
- Points every Ethernet and Wi-Fi adapter at a DNS provider (Quad9, Cloudflare or Google, depending on the profile) with encrypted DNS (DoH) required, checks with real lookups that it works, tries the next provider if not, and puts your old DNS back if nothing works. After DoH, **DNS over TLS (DoT) is tried as well, but it is experimental**: Windows documents no API for it, so every change is read back and a backup of Windows' encrypted-DNS server list is kept next to the apply backup and restored by `revert`. Needs Windows 11.

## Usage

```
netshutup-modern                    Open the menu
netshutup-modern scan               Show the current network settings
netshutup-modern preview <profile>  Show what a profile would change
netshutup-modern apply <profile>    Apply a profile (asks first, saves a backup)
netshutup-modern apply <profile> --dry-run
                                    Go through apply without changing anything
netshutup-modern revert             Undo the most recent apply
netshutup-modern selftest           Test every profile for real and write a report (for testers)
```

Profiles: `privacy`, `performance`, `recommended`.

`apply` and `revert` need administrator rights; the program asks for them through UAC only when one of those commands is run. From the menu, UAC is asked once and the menu continues in the administrator window.

Without arguments in a non-interactive context (a script, a redirect) the program prints the usage text instead of opening the menu.

## Tester mode (self-test)

For people helping to test: choose **Self-test (for testers)** in the menu, or run `netshutup-modern selftest`. After one confirmation and one administrator prompt it applies each profile for real, checks the result, undoes it, and checks that everything is back as it was. It then writes a report (`.md` and `.json`) to `%ProgramData%\NetShutUp\reports` and your Desktop and opens the folder.

- **Nothing is sent anywhere.** The report only exists on your computer; you send it yourself.
- It notes a limited set of diagnostic facts that help tell a bug from a hardware or network problem: Windows version, processor and memory, computer make and model, network adapter hardware and drivers, how many VPN or virtual adapters exist, whether the computer is on a domain or has DNS/telemetry policies, registered antivirus/firewall names, and whether a proxy is set. No user name, computer name, Wi-Fi name, full IP or MAC addresses or serial numbers; addresses are masked.
- **Safety net:** before anything is changed it exports the relevant registry keys to `%ProgramData%\NetShutUp\reg-backups` and creates a System Restore point (if System Protection is on and no point was made in the last 24 hours; otherwise that is noted in the report). Any problem is written into the report.
- If it is interrupted, the menu warns you and **Revert last change** puts everything back.
- Attach both report files to a new issue using the **Test report** form.

## Safety

- Nothing is changed without a preview and a confirmation.
- A backup is written to `%ProgramData%\NetShutUp\backups` and verified before the first change. If a write fails, everything done so far is rolled back.
- A setting that was not set before is removed again on revert, not written as zero.
- Per-adapter settings only go to physical Ethernet and Wi-Fi adapters. Virtual adapters (Wi-Fi Direct, WAN miniports, tunnels) are left alone.
- Windows Firewall settings are skipped when another firewall (for example an antivirus suite) is registered, so two firewalls never fight over the connection. Existing allow rules are not touched.
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

- Windows Firewall strict tier (Privacy profile)

Later: per-setting control (Advanced Mode) and a GUI.
