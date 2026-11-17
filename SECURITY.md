# Security Policy

NetShutUp changes Windows network settings and asks for administrator rights
to do so, so security reports are taken seriously.

The project is **pre-alpha** (see the README). Only the latest commit on `main`
is supported.

## Reporting a vulnerability

Please do not open a public issue for a security problem.

Use GitHub's private reporting instead: open the **Security** tab of this
repository and choose **Report a vulnerability**. Include:

- what you found and why it is a problem,
- the Windows version and build, and the NetShutUp commit or version,
- steps to reproduce it, if you have them.

You can expect an answer within a few days. This is a small open-source
project, so fixes are made on a best-effort basis.

## What is in scope

- Anything that lets a program or user without administrator rights make
  NetShutUp write something it should not (settings outside its catalog,
  files outside its backup folder).
- Backups that can be tampered with to make `revert` write unexpected values.
- Changes that are applied without the preview and confirmation.
- A revert that leaves the system different from before the apply.

Plain bugs (wrong text, a crash, a setting shown incorrectly) are not security
issues; please report them as normal issues.
