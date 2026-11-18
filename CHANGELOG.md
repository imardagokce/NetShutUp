# Changelog

All notable changes are listed here. The project follows [Semantic Versioning](https://semver.org/); versions before 1.0 may change freely.

## [Unreleased]

### Added
- Windows Firewall strict tier in the Privacy profile (four optional items, each asked separately, default No). Not in the `0.1.0-alpha.1` build.

## [0.1.0-alpha.1] - 2026-11-18

First test build, for testers. See [`docs/release-notes/v0.1.0-alpha.1.md`](docs/release-notes/v0.1.0-alpha.1.md).

### Added
- Profiles (Privacy, Performance, Recommended) with preview, dry-run, backup, apply and revert.
- Interactive terminal menu and the `scan`, `preview`, `apply`, `revert` and `selftest` commands.
- Encrypted DNS with DoH, real-lookup trial, provider fallback and automatic restore; experimental DoT.
- Windows Firewall base tier (on, inbound blocked by default), skipped when another firewall is registered.
- Self-test mode with a masked report, registry export and System Restore point.
