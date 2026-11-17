# NetShutUp

Windows network settings configurator - Privacy, Performance & Recommended profiles.

This repository is organized into two product lines:

- `modern/` — Windows 10/11 focused application. This is the default active branch (`main`).
- `legacy/` — legacy Windows branch kept in a separate `legacy` branch for older Windows compatibility work.
- `docs/` — shared planning, compatibility, and product design notes.

## Status

Pre-alpha, working towards `0.1.0-alpha.1` (command-line only, no GUI yet).

The Modern application can already show the current network settings, preview a profile, apply it with a backup, and revert it. The code that changes settings has so far only been exercised by tests, not on real systems. See [`modern/README.md`](modern/README.md) for usage and [`docs/design/`](docs/design/) for the design notes and roadmap.

## Principles

- Read-only diagnostics first: show the current state before proposing changes.
- Nothing is changed before the user reviews a preview and confirms.
- Every change is backed up first and can be reverted.
- A single `.exe`, no installer.

## Branch structure

- `main` — modern implementation track
- `legacy` — legacy implementation track

## Important note

Support claims are not final until the relevant OS builds are tested on a real machine or VM.

## License

GPL v3. See [`LICENSE`](LICENSE).
