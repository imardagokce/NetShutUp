# Contributing to NetShutUp

Thank you for helping. There are three good ways to contribute, and **testing is the most valuable one right now**.

| I want to... | Go to |
|---|---|
| Try NetShutUp on my computer and report what happened | [Testing guide](#testing-guide) |
| Report a bug or suggest an idea | [Reporting](#reporting-bugs-and-ideas) |
| Change the code or the docs | [Development guide](#development-guide) |

Security problems are handled privately, see [`SECURITY.md`](SECURITY.md).

---

## Testing guide

The full step-by-step guide for anyone, no programming knowledge needed, is **[`docs/TESTING.md`](docs/TESTING.md)** (Türkçe: [`docs/TESTING.tr.md`](docs/TESTING.tr.md)). It covers downloading and checking the file, the SmartScreen warning, running the self-test, what you will see, sending the report, and how to undo everything.

In short: download the release, run **Self-test (for testers)**, and attach the two report files to a [test report](https://github.com/imardagokce/NetShutUp/issues/new?template=test-report.yml). The self-test makes a System Restore point and registry export first, undoes every change it makes, and sends nothing over the network.

### If something goes wrong

- **The program says everything was NOT restored**, or the menu warns about an unfinished self-test: choose **Revert last change** in the menu (or run `netshutup-modern revert`). That puts everything back.
- **Internet does not work after a test:** run `netshutup-modern revert` first. If that is not enough, use **System Restore** (search "Create a restore point" in the Start menu, then *System Restore*, pick the one named "NetShutUp self-test"), or double-click the `.reg` files in the reg-backups folder named in the report.
- Then send us the report anyway; a failure is exactly what we need to see.

### Things we especially want to know

- Does the menu look right in your terminal (colours, arrow keys, Esc)?
- After the self-test, does Windows Settings show your DNS as before?
- Did the self-test say encrypted DNS worked, fell back, or failed? Which network (home, mobile hotspot, office)?
- Do you use a VPN, a proxy, a managed work computer or a third-party firewall? Tell us; these change results.

---

## Reporting bugs and ideas

- **Bug or test result:** use the **Test report** form (link above); it asks for the Windows build, what you ran and the output.
- **Idea or question:** open a normal issue and describe what you want and why.
- **Security problem:** use GitHub's private vulnerability reporting (see [`SECURITY.md`](SECURITY.md)), not a public issue.

---

## Development guide

### Setup

```sh
git clone https://github.com/imardagokce/NetShutUp.git
cd NetShutUp/modern
cargo test
cargo build --release
```

You need the stable Rust toolchain with the MSVC target on Windows. The C runtime is linked statically, so the release `.exe` runs without extra DLLs.

### Ground rules (these protect users)

- **No test may write to real system settings.** Registry tests use the sandbox key under `HKEY_CURRENT_USER\Software\NetShutUp-Test`; logic is tested against fakes (`FakeStore`, scripted resolvers). Code that touches the real system (the real `netsh`, `reg`, restore-point or DNS calls) is only compiled by tests, never run.
- **Back up before every change, and everything must be revertible.** A setting that was *not set* must come back as *not set*.
- **Do not parse localized text output** (Windows may be Turkish, German, ...). Use APIs, registry values or exit codes.
- **Only verified tweaks.** A setting enters a profile only if its registry path, default and effect were checked against Microsoft documentation.
- **Nothing is sent over the network** by the program.
- Program text is English.
- Never run `apply`, `revert` or `selftest` against your own machine while developing unless you mean to; use `preview` and `--dry-run`.

### Where things are

```text
modern/src/
  main.rs, commands.rs, menu.rs, terminal.rs   command line, menu, terminal
  settings/   catalog, profiles, reader, store, apply/backup engine, plan
  dns/        encrypted DNS trial chain (DoH, experimental DoT), resolver
  selftest/   tester mode: runner, report rendering, masking, safety net
modern/profiles/   built-in profiles (JSON, embedded in the .exe)
docs/design/       design notes: the source of truth for decisions
```

Start with [`modern/README.md`](modern/README.md) and the notes in [`docs/design/`](docs/design/).

### Workflow

1. Open an issue (or comment on one) before a larger change so we agree on the idea.
2. Fork, then create a branch for your change; open a pull request to `main`. The maintainers keep the repository tidy: only `main` and `legacy` live here, so please delete your branch after it is merged.
3. Write the failing test first, then the code. `cargo test` must pass and `cargo build --release` must have no warnings.
4. Keep the change small and focused. Update the relevant README or design note.
5. Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/): `feat:`, `fix:`, `refactor:`, `docs:`, `chore:`, `test:`.

### The two product lines

`main` is the Windows 10/11 line (`modern/`). The `legacy` branch (Windows 7/8/8.1) is a separate codebase; do not mix them.

### License

NetShutUp is licensed under the **GNU General Public License v3.0**. By contributing you agree that your contribution is released under the same license.

### Be kind

Be respectful and patient. Many testers are not developers; explain things plainly.
