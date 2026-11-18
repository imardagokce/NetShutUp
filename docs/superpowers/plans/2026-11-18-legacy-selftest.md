# Legacy Selftest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `netshutup-legacy selftest` (and a menu item) safely runs every profile through real apply -> verify -> revert -> compare on Win7/8/8.1 and writes a masked `.md` + `.json` report.

**Architecture:** Pure, unit-testable modules (`selftest_mask`, `selftest_report`) plus a runner that takes its side effects (registry snapshot, apply, revert, clock) through a small `Env` struct of function pointers, so tests use fakes and never touch the real registry. A thin layer in `selftest.cpp` wires the real engine (`apply::apply_profile`, `apply::revert`) and the safety net.

**Tech Stack:** C++20, Win32, MSVC, CMake/CTest, existing `apply`/`backup`/`catalog`/`registry`/`adapters` modules.

**Spec:** `docs/design/2026-11-17-selftest.md` on `main` (Rust version; DNS and firewall parts do not exist in legacy and are out of scope here). Behaviour below is that spec reduced to what legacy has.

## Global Constraints

- `/std:c++20 /W4 /WX /utf-8 /permissive-`, `_WIN32_WINNT=0x0601`, static CRT (`/MT`). Legacy line only; never touch `main`.
- FORBIDDEN (WaitOnAddress, crash on Win7): `std::atomic_wait`, `atomic_notify_*`, `std::latch`, `std::barrier`, `std::counting_semaphore`. No threads needed at all.
- No ANSI escapes. Program text is English. Never parse localized output (exit codes only).
- Tests never write to the real registry; sandbox only (`registry::SANDBOX_SUBKEY`).
- No network traffic, nothing is uploaded. Report is a local file only.
- Real `apply`/`revert` on Arda's machine only with explicit approval at that moment. Development uses fakes and `--dry-run`.
- Commits: Conventional Commits, trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` (user CLAUDE.md for `main` says no co-author; this session's attribution reminder wins for new commits, confirm with Arda if unsure).

## Review Focus

- Revert fails mid-run: runner stops, report says NOT everything was restored, prints manual command `netshutup-legacy revert <backup path>`. (Task 3)
- Snapshot differs after revert (a value changed or a "not set" value became 0): reported as FAIL with the differing catalog ids. (Task 3)
- Not elevated: refuse to write anything, report is not produced, exit code 1. (Task 4)
- Backup folder not writable / `reg.exe` missing: safety net step FAILs and selftest aborts before any write. (Task 4)
- Mask input contains IPv6, link-local, mixed case MAC with `-` or `:`, GUID in braces, `C:\Users\<name>\...` paths inside free text. (Task 1)

## File Structure

| File | Responsibility |
|------|----------------|
| `legacy/src/selftest_mask.h/.cpp` | Pure string maskers (IPv4, IPv6, MAC, GUID, user paths, `sanitize_text`) |
| `legacy/src/selftest_report.h/.cpp` | `Status`, `Check`, `Report` types; `render_markdown`, `render_json` (pure) |
| `legacy/src/selftest.h/.cpp` | `Env`, `run(Env&)`, real `make_real_env()`, safety net, report files, `cmd_selftest()` |
| `legacy/src/main.cpp` | Replace stub at the `selftest` branch (lines ~150-154) with a call to `selftest::cmd_selftest` |
| `legacy/src/menu.cpp` | Add item "Self-test (for testers)" |
| `legacy/CMakeLists.txt`, `legacy/tests/CMakeLists.txt` | Register new sources and tests |
| `legacy/tests/test_selftest_mask.cpp`, `test_selftest_report.cpp`, `test_selftest_runner.cpp` | Tests |
| delete | `legacy/src/selftest_tmp.h`; replace empty `selftest.cpp` / 1-line `selftest.h` |

---

### Task 0: Toolchain check and cleanup (do first, me, no subagent)

**Files:** delete `legacy/src/selftest_tmp.h`.

- [ ] **Step 1:** Find MSVC + CMake (`vswhere`, "Developer PowerShell"); record the exact configure/build/test commands in `legacy/build/README.md`'s neighbour note or here.
- [ ] **Step 2:** Run existing suite once: `cmake -S legacy -B legacy/build && cmake --build legacy/build --config Debug && ctest --test-dir legacy/build -C Debug`. Expected: 6 tests pass. If not, stop and report; do not build on a red base.
- [ ] **Step 3:** `git rm`/delete `selftest_tmp.h`, overwrite empty `selftest.cpp` and 1-line `selftest.h` (UTF-16 garbage) with nothing; commit `chore: remove selftest placeholders`.

### Task 1: Masking (subagent-friendly, haiku)

**Files:** Create `legacy/src/selftest_mask.h/.cpp`, `legacy/tests/test_selftest_mask.cpp`.

**Interfaces — Produces** (namespace `netshutup::selftest::mask`):
`std::wstring mask_ipv4(const std::wstring&)`, `mask_ipv6`, `mask_mac`, `mask_guid`, `mask_user_paths`, `sanitize_text(const std::wstring&)` (applies all, in a fixed order, to free text).

- [ ] **Step 1: Failing tests** (plain `main()` returning nonzero on failure, same style as `test_adapters.cpp`):
  - `1.1.1.1`, `1.0.0.1`, `8.8.8.8`, `8.8.4.4`, `9.9.9.9`, `149.112.112.112` stay; `10.0.3.7` -> `10.0.x.x`; `192.168.1.20` -> `x.x.x.x`; `203.0.113.5` -> `x.x.x.x`.
  - `fe80::1c2:3ff:fe4:5` -> `x:x::x`; `2606:4700:4700::1111` (Cloudflare) stays.
  - `AA-BB-CC-11-22-33` and `aa:bb:cc:11:22:33` -> keep first 3 bytes, rest `xx` (`AA-BB-CC-xx-xx-xx`).
  - `{4C1E9A02-1111-2222-3333-444455556666}` -> `{4C1E9A02-xxxx-xxxx-xxxx-xxxxxxxxxxxx}`.
  - `C:\Users\Arda\AppData\x.txt` and `D:\Users\Bob\Desktop` -> user name replaced with `<user>`.
  - `sanitize_text(L"fail at 192.168.0.4 for C:\\Users\\Ayse\\a")` contains neither `192.168.0.4` nor `Ayse`.
- [ ] **Step 2:** Add target `test_selftest_mask` to `tests/CMakeLists.txt` (+ source in `LEGACY_LIB_SOURCES`); build; Expected: FAIL (link error / assertions).
- [ ] **Step 3:** Implement with hand-written scanners (no `<regex>`: slow and heavy; plain loops).
- [ ] **Step 4:** Run `ctest -C Debug -R selftest_mask`. Expected: PASS.
- [ ] **Step 5:** Commit `feat: add selftest masking helpers`.

### Task 2: Report model and renderers (subagent-friendly, haiku)

**Files:** Create `legacy/src/selftest_report.h/.cpp`, `legacy/tests/test_selftest_report.cpp`.

**Interfaces — Produces** (namespace `netshutup::selftest`):
```cpp
enum class Status { Pass, Fail, Skip, Info };
struct Check  { std::wstring id, title, detail; Status status; unsigned long millis; };
struct Section{ std::wstring name; std::vector<Check> checks; };   // "Environment", "privacy", ...
struct Report { int schema = 1; std::wstring started_utc; unsigned long total_millis = 0;
                bool all_restored = false; std::wstring manual_revert_cmd; std::vector<Section> sections; };
std::wstring render_markdown(const Report&);
std::string  render_json(const Report&);   // UTF-8
```
Every free-text field passes through `mask::sanitize_text` inside the renderers (Task 1 dependency; if Task 1 is not merged yet, call a local forwarding stub declared in `selftest_mask.h`).

- [ ] **Step 1: Failing tests:** (a) `all_restored=false` -> markdown starts with a line containing `NOT everything was restored` and the manual command; `true` -> contains `All settings restored: yes`. (b) summary line counts `✅ n  ❌ n  ⏭ n`. (c) JSON has `"schema":1`, escapes `"` and `\` in detail, and contains no key outside `{schema, started_utc, total_millis, all_restored, manual_revert_cmd, sections, id, title, detail, status, millis, name, checks}`. (d) detail `"IP 192.168.1.9"` is masked in both outputs.
- [ ] **Step 2-4:** Add test target, run red, implement (hand-written JSON writer, UTF-8 conversion via `WideCharToMultiByte`), run green.
- [ ] **Step 5:** Commit `feat: add selftest report model and renderers`.

### Task 3: Runner with injectable Env (me or sonnet; the safety-critical part)

**Files:** Create `legacy/src/selftest.h/.cpp` (runner part), `legacy/tests/test_selftest_runner.cpp`.

**Interfaces — Produces:**
```cpp
using Snapshot = std::vector<std::pair<std::wstring, std::optional<DWORD>>>; // catalog id -> value|not set
struct Env {
  Snapshot (*snapshot)(void*);                                  // all GLOBAL catalog entries
  apply::ApplyResult (*apply)(void*, const wchar_t* profile);
  apply::RevertResult (*revert)(void*, const std::wstring& backup_path);
  unsigned long (*now_ms)(void*);
  void* ctx;
};
Report run(const Env& env, const std::vector<std::wstring>& profiles);
```
Rules: for each profile -> snapshot, apply, snapshot (must differ for at least the profile's non-default entries, else Info "nothing changed"), revert, snapshot, compare with the first. Apply fail -> Fail, attempt revert, go to next profile. Revert fail -> Fail, **stop**, `all_restored=false`, `manual_revert_cmd = "netshutup-legacy revert <backup>"`. Final snapshot vs very first: any diff -> Fail listing ids, `all_restored=false`.

- [ ] **Step 1: Failing tests with a `FakeEnv`** (in-memory map): `all_green` (3 profiles, `all_restored==true`, no Fail); `apply_fails_continues` (profile 1 apply fails -> its section has Fail, profiles 2-3 still run); `revert_fails_stops` (profile 2 revert fails -> profile 3 section absent, `all_restored==false`, manual command contains backup path); `diff_after_revert` (fake revert turns "not set" into 0 -> Fail naming that id); `not_set_stays_not_set` (value absent before/after passes).
- [ ] **Step 2:** red. **Step 3:** implement `run`. **Step 4:** green. **Step 5:** commit `feat: add selftest runner`.

### Task 4: Real wiring, safety net, files, CLI + menu (me)

**Files:** Modify `legacy/src/selftest.cpp`, `main.cpp` (stub at `// ── selftest`), `menu.cpp`, `CMakeLists.txt`.

**Interfaces — Produces:** `int selftest::cmd_selftest(const OsVersion&, int argc, wchar_t** argv)`; `bool selftest::create_safety_net(std::wstring& out_reg_path, std::wstring& err)`.

- [ ] **Step 1:** `make_real_env()`: snapshot via `registry::read_dword` over `catalog::entries()` (GLOBAL scope only; PER_ADAPTER is skipped by backup too), apply/revert delegate to `apply::apply_profile(profile, os_mask)` / `apply::revert(path)`.
- [ ] **Step 2:** Safety net: exit code only. `reg.exe export "HKLM\SYSTEM\CurrentControlSet\Services\Tcpip" <file> /y` and the other hives/keys the catalog touches (derive list from the catalog's distinct hive+subkey roots); then `SRSetRestorePointW` loaded via `LoadLibraryW(L"srclient.dll")`/`GetProcAddress` (absent or disabled -> Skip with reason, not Fail). Reg export failure -> abort before any write.
- [ ] **Step 3:** `cmd_selftest`: confirmation prompt (default No, three English lines incl. "internet may drop briefly", "everything is reverted", "nothing is sent anywhere"); `--yes` skips it for scripted testers; if not elevated -> `elevation::restart_elevated`; Environment section (Windows version, arch, locale name, elevation, backups dir writable, physical adapter count via `adapters`); run; write `NetShutUp-selftest-<YYYYMMDD-HHMMSS>.md/.json` to `%ProgramData%\NetShutUp\reports\` and desktop copy (`SHGetFolderPathW(CSIDL_DESKTOPDIRECTORY)`); print paths; `ShellExecuteW(L"open", folder)`. Exit 0 only if no Fail and `all_restored`.
- [ ] **Step 4:** `main.cpp`: replace stub with `return selftest::cmd_selftest(os, argc, argv);`. `menu.cpp`: add item 8 "Self-test (for testers)" before Exit; keep Exit last (renumber the switch).
- [ ] **Step 5:** `CMakeLists.txt`: add `src/selftest.cpp src/selftest_mask.cpp src/selftest_report.cpp`; link `shell32 ole32 shlwapi` if needed.
- [ ] **Step 6: Verify without touching real settings:** build; `netshutup-legacy selftest` in a non-elevated shell must print the elevation message (don't accept UAC); `netshutup-legacy preview privacy` and `apply privacy --dry-run` unchanged; full `ctest` green.
- [ ] **Step 7:** Commit `feat: wire selftest command and menu item`.

### Task 5: Docs (haiku)

**Files:** Modify `GEMINI.md` (status + structure + commands), `CHANGELOG.md`, `README.md`/`legacy/README.md` command list; add `docs/TESTING-legacy.md` (tester guide: run as admin, what it does, where reports land, how to send, manual revert command).

- [ ] **Step 1:** Update; **Step 2:** commit `docs: document legacy selftest`.

---

## Subagent plan (cost first, per Arda's rule)

| Task | Who | Model |
|------|-----|-------|
| 0, 3, 4 | me (safety-critical, shared files) | session model |
| 1 | subagent A (disjoint files) | haiku |
| 2 | subagent B (disjoint files) | haiku |
| 5 | subagent C after 4 | haiku |

1 and 2 run in parallel; I own every CMake edit to avoid conflicts. Final: one review pass (`code-review`) on the diff.

## Out of scope

DNS/DoH/DoT (no Win7 support), firewall (Tier 1 is a separate plan), GUI, any upload.
