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

### #002 — Malformed frame: reason not logged

- **Date:** 2026-11-23
- **Found by:** Nghĩa (test report row 3)
- **Severity:** minor
- **Steps to reproduce:**
  1. Start the server: `./build/caro_server 5500`.
  2. Run `./build/cli 127.0.0.1 5500` and type `/zero` (or `/huge` in a new cli).
  3. Look at the server log.
- **Expected:** the server closes that connection and writes a log line that gives the reason
  (docs/protocol.md §1: "A frame with LENGTH 0 or larger than 16384 is malformed: the server
  logs it and closes that connection").
- **Actual:** the connection is closed correctly, but the log only has
  `EVENT | disconnected: <addr>`.
- **Suggested fix:** in `server/main.c`, in the `frame_result < 0` branch, call
  `log_event("malformed frame from %s", c->addr)` before `session_end(c)`.
- **Status:** FIXED eec96ce

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