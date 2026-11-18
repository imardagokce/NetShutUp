# Contributing to NetShutUp — Legacy

Thank you for helping. There are three good ways to contribute.

> [!NOTE]
> This guide covers the **Legacy** branch (Windows 7/8/8.1, C++20 / Win32).
> For the Modern line (Windows 10/11, Rust), see the `main` branch.

| I want to… | Go to |
|---|---|
| Try NetShutUp Legacy on my computer and report what happened | [Testing guide](#testing-guide) |
| Report a bug or suggest an idea | [Reporting](#reporting-bugs-and-ideas) |
| Change the code or the docs | [Development guide](#development-guide) |

Security problems are handled privately — see [`SECURITY.md`](SECURITY.md).

---

## Testing guide

> [!IMPORTANT]
> No binary is available yet. This section describes the intended testing workflow once the first build is released. Watch the repository for release announcements.

When testing becomes available:

1. Download the release `.exe` — no installer needed.
2. Run it. If Windows SmartScreen warns you, click **More info → Run anyway** (the binary is unsigned for now).
3. Choose **Self-test (for testers)** in the menu, or run `netshutup-legacy selftest`.
4. Accept the UAC prompt.
5. The self-test applies each profile, verifies it, undoes it, and writes a masked report. It sends nothing over the network.
6. [Open a test report issue](https://github.com/imardagokce/NetShutUp/issues/new) and attach the report files.

### If something goes wrong

- **Menu warns about an unfinished self-test:** choose **Revert last change** in the menu (or run `netshutup-legacy revert`). That puts everything back.
- **Network does not work after a test:** run `netshutup-legacy revert` first. If that is not enough, use **System Restore**.
- Send the report anyway — a failure is exactly what we need to see.

### Things we especially want to know

- Which exact Windows version and edition are you running (7 SP1 / 8 / 8.1, x86 or x64)?
- Does the menu display correctly in your terminal (colours, arrow keys)?
- Did the self-test say everything was restored correctly?
- Do you use a VPN, proxy, managed work computer or third-party firewall? Tell us — these affect results.

---

## Reporting bugs and ideas

- **Bug or test result:** open a GitHub issue using the test report template.
- **Idea or question:** open a normal issue and describe what you want and why.
- **Security problem:** use GitHub's private vulnerability reporting (see [`SECURITY.md`](SECURITY.md)), not a public issue.

---

## Development guide

### Setup and Building

The Legacy project builds as a standalone single executable using CMake and MSVC:

```sh
git clone https://github.com/imardagokce/NetShutUp.git
git checkout legacy
cmake -B legacy/build -S legacy
cmake --build legacy/build --config Release
ctest --test-dir legacy/build -C Release --output-on-failure
```

You will need **Visual Studio 2019+** or **Build Tools** (MSVC toolset supporting **C++20**) with the Windows SDK. The build automatically links the C/C++ Runtime statically (`/MT`) so the generated `.exe` runs on clean Windows 7 SP1 systems without any installed VC++ Redistributable.

### C++20 Standard & Windows 7 Restrictions

While the project uses **C++20** (`/std:c++20`), certain MSVC STL synchronization primitives rely internally on `WaitOnAddress`, an API that only exists in Windows 8 and later. Therefore, the following features are **strictly banned** project-wide:

- `std::atomic_wait` / `std::atomic_notify_*`
- `std::latch`
- `std::barrier`
- `std::counting_semaphore`

See [ADR-001](docs/design/ADR-001-legacy-language-selection.md) for full rationale and allowed alternatives.

### Ground rules (these protect users)

- **No test may write to real system settings.** Registry tests use a sandbox key under `HKEY_CURRENT_USER\Software\NetShutUp-Test`; logic is tested against fakes. Code that touches real system settings is never run by the test suite.
- **Back up before every change, and everything must be revertible.** A setting that was *not set* must come back as *not set*.
- **Do not parse localized text output.** Windows may be in Turkish, German, or any other language. Use Win32 APIs, registry values, or exit codes — never `cmd /c` output strings.
- **Only verified tweaks.** A setting enters a profile only if its registry path, default value and effect were checked against Microsoft documentation and confirmed to exist on Windows 7/8/8.1.
- **Nothing is sent over the network** by the program.
- Program text is always **English**.
- Never run `apply`, `revert` or `selftest` against your own machine while developing unless you explicitly intend to; use `preview` and `--dry-run`.
- Use `RtlGetVersion` for OS version detection — `GetVersionExW` returns wrong results on Windows 8.1+ due to the compatibility shim.

### Where things will be

```text
legacy/
  src/           application source (C++20 / Win32)
  profiles/      built-in profiles (embedded in the .exe)
  tests/         unit and integration tests
  CMakeLists.txt build definition
docs/
  design/        Architecture Decision Records — source of truth for decisions
  compatibility-matrix.md
```

Start with [`docs/design/`](docs/design/) and the notes in [`docs/compatibility-matrix.md`](docs/compatibility-matrix.md).

### Workflow

1. Open an issue (or comment on one) before a larger change so we agree on the idea.
2. Fork, then create a branch from `legacy` for your change; open a pull request back to `legacy`. Delete your branch after it is merged — the repository keeps only `main` and `legacy`.
3. Write the failing test first, then the code. All tests must pass and the build must have no warnings.
4. Keep the change small and focused. Update the relevant design note or compatibility matrix entry.
5. Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/): `feat:`, `fix:`, `refactor:`, `docs:`, `chore:`, `test:`.
6. Run `git status` before every commit, and **push** — the repository is worked on from more than one machine.

### The two product lines

`legacy` (this branch) is the Windows 7/8/8.1 line. `main` is the Windows 10/11 line (Rust). They are **completely independent** — do not mix them, do not share code between them.

### License

NetShutUp is licensed under the **GNU General Public License v3.0**. By contributing you agree that your contribution is released under the same license.

### Be kind

Be respectful and patient. Many testers are not developers; explain things plainly.
