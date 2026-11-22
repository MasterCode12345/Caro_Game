# Cross-team issues

A bug found in the other owner's folder is written here instead of being fixed directly
(`CLAUDE.md`, "Team and ownership"). Newest issue at the top. The owner who fixes it updates
**Status** and names the commit.

Status values: `OPEN` · `FIXED <commit>` · `WONTFIX <reason>`

## Template

Copy this block to the top of the list below and fill it in.

```markdown
### #NNN — <short title>

- **Date:** YYYY-MM-DD
- **Found by:** Nghĩa | Hân
- **Steps to reproduce:**
  1. …
  2. …
- **Expected:** … (cite the docs/protocol.md section)
- **Actual:** … (paste the cli output, CARO_DEBUG=1 log or server.log lines)
- **Status:** OPEN
```

## Issues

### #001 — session_end() recursion when clients disconnect together

- **Date:** 2026-11-22
- **Found by:** Nghĩa (bot C6a)
- **Steps to reproduce:**
  1. Run the server at the S1 version.
  2. Log in with `./build/bot 127.0.0.1 5500 10`.
  3. Stop the bot, so all 10 clients disconnect at the same time.
- **Expected:** the server keeps running (docs/protocol.md §8).
- **Actual:** SIGSEGV from stack overflow: `session_end()` and `broadcast_status()` call each
  other (ASan: net.c:173 / net.c:198 / lobby.c:65), 3 of 3 runs.
- **Status:** FIXED 1bdcd6e