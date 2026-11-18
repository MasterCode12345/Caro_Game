# Caro Online — Group 13

Turn-based online Caro (Gomoku) for the Network Programming course (IT4062E, SoICT HUST).
15×15 board, X moves first, five or more in a row wins, a full board is a draw.

- **Server**: C, one process, one `select()` loop, SQLite storage. Target: Linux.
- **Client**: C + GTK 3, single-threaded (GLib main loop). Developed on macOS, builds on Ubuntu.
- **Protocol**: our own, over plain TCP sockets, length-prefixed frames — see
  [`docs/protocol.md`](docs/protocol.md).

Status: project skeleton. Stage-by-stage progress is in [`docs/progress.md`](docs/progress.md).

## Install

**Ubuntu**

```bash
sudo apt install build-essential pkg-config libgtk-3-dev libsqlite3-dev gdb valgrind
```

**macOS** (Homebrew; SQLite ships with macOS)

```bash
xcode-select --install
brew install gtk+3 pkg-config
```

Only the client needs GTK: `make server` works without it.

## Build and run

```bash
make                       # everything into build/
./build/caro_server 5500   # server (default port 5500)
./build/caro_client        # GTK client
./build/cli 127.0.0.1 5500 # line client for manual protocol testing
```

| Command | Builds / does |
| --- | --- |
| `make` | all binaries below |
| `make server` | `build/caro_server` |
| `make client` | `build/caro_client` |
| `make tools` | `build/cli`, `build/bot`, `build/test_frame` |
| `make test` | builds and runs `test_frame`, `test_game`, `test_elo` |
| `make debug` | everything with `-O0 -fsanitize=address,undefined` |
| `make DEBUG=1 test` | the tests with the sanitizers |
| `make clean` | removes `build/` |

Switching between `make debug` and `make` rebuilds every object automatically.
The policy is zero compiler warnings (`-std=c11 -Wall -Wextra`).

## Team

| Folder | Owner |
| --- | --- |
| `client/` — GTK 3 client | Nghĩa |
| `tools/` — `cli`, `bot`, `test_frame` | Nghĩa |
| `common/` — `proto.h`, `frame.h/.c` (shared) | Nghĩa writes, both use; changes agreed by both |
| `server/` — C server and its unit tests | Hân |
| `docs/protocol.md` | both: change the doc first, then the code |

Working rules (details in [`CLAUDE.md`](CLAUDE.md)):

- Do not edit the other owner's folder. Report the bug in [`docs/issues.md`](docs/issues.md).
- Commit prefixes: `client:`, `server:`, `common:`, `protocol:`, `docs:`. A change to
  `common/` or `docs/protocol.md` uses `protocol:` and must be told to the other member.
- Update `docs/progress.md` at the end of every session.
