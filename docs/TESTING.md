# How to test NetShutUp (step by step)

*[Türkçe sürüm](TESTING.tr.md)*

Thank you for helping. You do **not** need to be a developer. This guide takes about **10–15 minutes** and tells you exactly what to click, what you should see, and what to do if something looks wrong.

## What you need

- A Windows 10 or Windows 11 computer (Windows 11 lets us test encrypted DNS too).
- An administrator account (Windows will show a normal permission prompt).
- Ideally **your own computer**, not a work or school one (those may be managed and behave differently; if you test on one anyway, tell us in the report).
- A few minutes when you do not need the internet. It may drop for a moment during the test.

## Is it safe?

The test changes some network settings, **checks them, and then undoes every change**. To protect you:

- Before changing anything it makes safety copies: an export of the relevant registry keys and a Windows **System Restore point**.
- Every change is backed up first and can be reverted with one menu entry.
- If anything cannot be restored, the program says so loudly and tells you how to put things back (see [If something goes wrong](#if-something-goes-wrong)).
- **Nothing is sent over the internet.** It only writes a report file on your computer, and you decide whether to share it.

If you would rather not change anything, you can do only [the read-only look](#step-3-optional-take-a-read-only-look) and send us what you saw.

---

## Step 1 — Download and check the file

1. Open the [latest release page](https://github.com/imardagokce/NetShutUp/releases) and download:
   - `NetShutUp-<version>-windows-x64.zip`
   - `NetShutUp-<version>-windows-x64.zip.sha256`
2. *(Recommended)* Check that the download is intact. Open **PowerShell** in the folder where you saved the files and run (replace the file name with yours):

   ```powershell
   (Get-FileHash .\NetShutUp-v0.1.0-alpha.1-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
   Get-Content .\NetShutUp-v0.1.0-alpha.1-windows-x64.zip.sha256
   ```

   The long text printed by the first command must be the same as the start of the second line. If it is different, download again.
3. Right-click the zip → **Extract All…** and choose a folder (for example your Documents).

## Step 2 — Start the program

1. Open the extracted folder and double-click **`netshutup-modern.exe`**.
2. Windows may show a blue box **"Windows protected your PC"**. This is expected: the program is new and not code-signed yet. If the checksum matched in Step 1, click **More info → Run anyway**.
3. A terminal window opens with a menu. It looks like this:

   ```text
   NetShutUp 0.1.0-alpha.1 · Windows 11 (build 26200) · not elevated

   ? What would you like to do?
   > Scan current settings
     Apply a profile
     Revert last change
     Self-test (for testers)
     Exit
   ```

   Use the **↑ ↓ arrow keys** to move, **Enter** to choose, **Esc** to go back.

   *Does it look right in your terminal (colours, arrow keys)? If not, tell us in the report.*

## Step 3 — (Optional) Take a read-only look

Choose **Scan current settings**. It lists your network adapters and the current value of every setting NetShutUp knows. Nothing is changed and no administrator rights are needed. Press an arrow key to return to the menu.

To see what a profile would do without doing it, choose **Apply a profile → Privacy** (or another one). If it asks for administrator rights, you can say *No*: the preview is only a preview.

## Step 4 — Run the self-test

1. In the menu choose **Self-test (for testers)**.
2. Windows asks for permission to run as administrator. Click **Yes**. A **new window** opens and continues there.
3. Read the short explanation. It says what will happen and what information goes into the report. The default answer is **No**; type `y` and press Enter to continue.
4. Wait. The program prints one line for each step. It will:
   1. collect basic facts about your computer (no names, no passwords),
   2. make the safety copies (registry export and restore point),
   3. check that every setting can be read,
   4. for each profile (**Privacy**, **Performance**, **Recommended**): apply it, check it, test encrypted DNS, **undo it**, and check that everything is back as before,
   5. write the report.

   This takes a few minutes. Your internet may drop for a second or two. **Do not close the window** until it says the report was written.
5. At the end it prints something like:

   ```text
   Self-test finished: 38 passed, 0 failed, 3 skipped, 6 notes.

   Report written:
     C:\ProgramData\NetShutUp\reports\NetShutUp-selftest-20261118-120000.md
     ...
   ```

   and opens the folder with the report.

   If instead you see **"NOT everything was restored"**, go straight to [If something goes wrong](#if-something-goes-wrong).

## Step 5 — Look at the report and send it

1. Open the `.md` file (it opens in Notepad or any text/markdown viewer). At the top you see a summary (✅ passed, ❌ failed, ⏭ skipped, ℹ️ notes) and **"Everything restored: yes"**.
2. Skim it. It should contain **no** user name, computer name, Wi-Fi name, or full IP/MAC addresses (addresses are masked like `192.168.x.x`). It does contain hardware and network facts that help us understand problems; the section **"What this report contains"** lists them.
3. Go to **[New test report](https://github.com/imardagokce/NetShutUp/issues/new?template=test-report.yml)** (a free GitHub account is needed; if you do not want one, send the two files to whoever asked you to test).
4. Fill in the form: Windows build (it is printed in the report and in the menu title), what you ran, the result.
5. **Attach both files**: the `.md` and the `.json` (drag them into the comment box).
6. Add anything that surprised or confused you. Even "it all worked" is a useful report!

---

## If something goes wrong

Work through these in order and stop as soon as things are fine.

1. **Use the program's own undo.** Start `netshutup-modern.exe`, choose **Revert last change**, accept the administrator prompt, confirm. This puts everything back from the backup. (A warning at the top of the menu, "A previous self-test did not leave your settings as they were", also means you should do this.)
2. **Internet not working?** Restart the computer, then try the step above again.
3. **Still wrong?** Use the System Restore point the test created:
   1. Press **Start**, type **Create a restore point**, open it.
   2. Click **System Restore…**, then **Next**.
   3. Choose the point named **"NetShutUp self-test"** and finish the wizard.
   *(If no such point exists, System Protection was off or a point had been made in the last 24 hours; the report says which.)*
4. **Last resort:** the test also saved registry exports (`.reg` files) in `C:\ProgramData\NetShutUp\reg-backups\`. Double-clicking a file merges its saved values back. Only do this if you are comfortable with it, or ask us first.
5. **Then send us the report anyway.** A failure is the most valuable thing you can send.

## Questions

**Do I need to keep the program afterwards?** No. Delete the extracted folder. Reports stay in `C:\ProgramData\NetShutUp\reports` (and your Desktop) and backups in `C:\ProgramData\NetShutUp\backups`; you may delete them when you are done.

**My antivirus complains.** The program is new and unsigned, and it changes system settings by design, which can look suspicious. Check the checksum (Step 1). If you are not comfortable, build it from source (see the README) or skip the test.

**I use a VPN, a proxy, a company-managed computer, or a third-party firewall.** You can still test, but please mention it in the form; these change the results (for example a VPN can hide whether encrypted DNS worked).

**It says encrypted DNS "did not work on this network".** That is not necessarily a bug: some networks (offices, schools, some hotspots) block it, and the program then restores your old DNS. Tell us which kind of network you were on.

**What does it do to my DNS?** It points your Ethernet/Wi-Fi adapters at a privacy-friendly provider (Quad9, Cloudflare or Google) with encrypted DNS, tests it with real lookups, and restores your old DNS if it does not work. During the self-test everything is restored at the end.

**Can I test on Windows 10?** Yes. Everything except encrypted DNS is tested; that part is skipped on Windows 10.

**Can I run it more than once?** Yes. Each run is independent.

**Where can I ask for help?** Open an issue on GitHub, or write to whoever asked you to test.

## What to tell us (very helpful)

- Does the menu look and respond correctly (colours, arrow keys, Esc)?
- After the test, is your internet and DNS as before (Settings → Network & internet → your connection → DNS server assignment)?
- Which network were you on (home, mobile hotspot, office)?
- Anything confusing in the wording or the steps.

Thank you! More about contributing: [`CONTRIBUTING.md`](../CONTRIBUTING.md).
