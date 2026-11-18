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

NetShutUp is **pre-alpha**. The part that changes settings has been tested with unit tests but not yet on many real computers. Your test is what proves it works.

### Before you start

- Use **Windows 11** if you can. Everything except encrypted DNS also works on Windows 10; on Windows 10 the DNS step is skipped.
- Close important work. During the self-test your internet may drop for a moment.
- You need an administrator account (Windows will show a normal permission prompt).
- It is safe to use your own computer: everything the self-test changes is undone, and it creates safety copies first (see below). If you are not comfortable, run only the read-only commands.

### 1. The safe start: read-only commands

These change nothing and need no administrator rights:

```text
netshutup-modern scan
netshutup-modern preview privacy
netshutup-modern preview performance
netshutup-modern preview recommended
netshutup-modern apply recommended --dry-run
```

Do the numbers and values look right for your computer? If anything looks wrong or confusing, that is a useful report already.

### 2. The self-test

Double-click the program (or run `netshutup-modern`) and choose **Self-test (for testers)**. Or run `netshutup-modern selftest`.

What happens, in order:

1. You read a short explanation and confirm (the default answer is **No**).
2. Windows asks for administrator permission.
3. **Safety net first:** the program exports the relevant registry keys to `%ProgramData%\NetShutUp\reg-backups` and creates a **System Restore point** (if System Protection is on and none was made in the last 24 hours; otherwise the report says so).
4. For each profile (Privacy, Performance, Recommended) it applies the settings for real, checks them, tests encrypted DNS, **undoes everything** and checks that the computer is exactly as before.
5. It writes a report (`.md` and `.json`) to `%ProgramData%\NetShutUp\reports` and your Desktop, and opens the folder.

It takes a few minutes. Nothing is uploaded anywhere.

### 3. Send the report

1. Open the `.md` file and read it. It should contain no user name, computer name, Wi-Fi name or full IP/MAC addresses (addresses are masked). It does contain hardware and network facts that help us understand problems (listed in the report under "What this report contains").
2. [Open a test report](https://github.com/imardagokce/NetShutUp/issues/new?template=test-report.yml) and attach **both** files. Add anything that surprised you.

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
