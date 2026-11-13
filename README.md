# NetShutUp

Windows network settings configurator - Privacy, Performance & Balanced profiles.

This repository is organized into two product lines:

- `modern/` — Windows 10/11 focused application. This is the default active branch (`main`).
- `legacy/` — legacy Windows branch kept in a separate `legacy` branch for older Windows compatibility work.
- `docs/` — shared planning, compatibility, and product design notes.

## Current project direction

- Start with a read-only diagnostics phase.
- Do not modify network settings before the user reviews the proposed changes.
- Maintain explicit user confirmation before applying any profile.
- Keep rollback and restore-point guidance in the application flow.

## Branch structure

- `main` — modern implementation track
- `legacy` — legacy implementation track

## Important note

The project is in the planning and architecture stage. Support claims are not final until the relevant OS builds are tested on a real machine or VM.
