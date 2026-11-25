# Caro Online — Group 13

Turn-based online Caro (Gomoku) for the Network Programming course (IT4062E, SoICT HUST).
15×15 board, X moves first, five or more in a row wins, a full board is a draw.

- **Server**: C, one process, one `select()` loop, SQLite storage. Target: Linux.
- **Client**: C + GTK 3, single-threaded (GLib main loop). Developed on macOS, builds on Ubuntu.
- **Protocol**: our own, over plain TCP sockets, length-prefixed frames — see
  [`docs/protocol.md`](docs/protocol.md).

Status (24/11/2026): milestones M0–M5 reached. Client stages C0–C6 and server stages S0–S8 are
done: every graded feature works, and the server has been checked with ASan/UBSan, `leaks`,
malformed requests and a 5-minute load run. Left: the two-machine LAN test. Checkpoint 2 ≈
01/12/2026. Stage-by-stage progress is in [`docs/progress.md`](docs/progress.md), the test table
in [`docs/test_report.md`](docs/test_report.md).

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

## Server

```bash
./build/caro_server                        # port 5500, database caro.db
./build/caro_server 5500                   # another port as first argument
./build/caro_server 5500 /tmp/test.db      # another database as second argument
```

- **Files**: `caro.db` (SQLite: users, matches, moves) and `server.log` are created in the
  **directory the server is started from**, so start it from the repository root. The second
  argument changes the database path; the log is always `server.log` in the current directory.
  Both are in `.gitignore`. Data survives a restart; the log is appended to, never cleared.
- **Log**: one line per request, `time | IP:port | user or - | command | status`, and `EVENT`
  lines (connections, game start and end, malformed frames, …). Arguments are never logged, so
  no password is. The same lines go to the terminal. During the demo, in a second terminal:

  ```bash
  tail -f server.log
  ```

- **Stop**: Ctrl+C (or `kill <pid>`, SIGTERM) stops cleanly. The server finishes the request it
  is handling, closes every client socket, the listening socket and the database, logs
  `server stopped` and exits with code 0. Clients go back to the login page. Games still running
  at that moment are dropped and not rated; finished games are already saved.
- **Start the demo with no data**: stop the server first, then, in the same directory:

  ```bash
  rm caro.db server.log
  ```

  This deletes every account, match and ELO. The next start creates an empty database.
- Under a load test, redirect the terminal output (`./build/caro_server 5500 > /dev/null`):
  printing thousands of lines per second to a terminal slows the server down.

## Chạy client

```bash
./build/caro_client                            # normal: connects to the server typed on the login page
CARO_FAKE=1 ./build/caro_client                # offline: fake.c plays the server, no socket is opened
CARO_DEBUG=1 ./build/caro_client               # print every message sent/received on stderr
CARO_FAKE=1 CARO_DEBUG=1 ./build/caro_client   # both
```

- **Login page**: Server (default `127.0.0.1`), Port (default `5500`), username, password.
  The format is checked before anything is sent (docs/protocol.md §2): username 3–20 characters
  `[A-Za-z0-9_]`, password 6–32 printable ASCII characters, no space, no `|`. Enter in any field
  logs in. Every error code and reason token of docs/protocol.md §4 is shown as a Vietnamese
  sentence; an unknown one as `Lỗi <code>: <reason>`.
- **Window**: the title shows who is logged in; the window cannot be made smaller than 760×560.
  Closing it during a running game asks first ("Thoát bây giờ sẽ bị xử thua"): the server
  counts the closed connection as a loss by `DISCONNECT`.
- **`CARO_FAKE=1`** — the window title ends with `[offline]`. Replies arrive after 300 ms through
  the same `dispatch()` path as real messages, so the UI cannot tell the difference. Test inputs:

  | Input | Fake reply | Message shown |
  | --- | --- | --- |
  | password `wrong` | `401` `invalid_credentials` | Sai tên đăng nhập hoặc mật khẩu |
  | register username `taken` | `409` `username_taken` | Tên đã có người dùng, hãy chọn tên khác |
  | login username `online` | `409` `already_logged_in` | Tài khoản đang đăng nhập ở nơi khác |
  | anything else | `201` (register) / `200`, ELO 1000 (login) | Đăng ký thành công… / lobby page |

- **`CARO_DEBUG=1`** — one line per message, for the demo:
  `[21:04:05.123] >> LOGIN|nghia|******` then `[21:04:05.125] << 200|LOGIN|nghia|1000|0|0|0`.
  Passwords are always masked. Warnings (a response that does not match the waiting request,
  an unknown event) are printed even without `CARO_DEBUG`.
- Two clients on one machine: just start `./build/caro_client` twice.

## Test server bằng cli

`./build/cli <host> <port>` opens one TCP connection. Each line you type is sent as **one
frame**, and every frame that arrives is printed at once as `<< payload`, including events the
server pushes while you are not typing (cli uses `select()` on stdin and the socket).

| Type | cli sends | A correct server (docs/protocol.md §1) |
| --- | --- | --- |
| `PING` (any line) | one frame with that line as payload | `<< 200\|PING` |
| `/bytes <text>` | one frame, **one byte at a time**, 50 ms apart (cut frame) | one response, after the last byte |
| `/burst <a> ;; <b> ;; <c>` | several frames **in a single `send()`** (glued frames) | one response per frame, in order |
| `/zero` | a header with LENGTH 0 | logs it and closes the connection |
| `/huge` | a header announcing LENGTH 20000, no payload | logs it and closes at once (no waiting for 20000 bytes) |
| `/help` | nothing: lists the commands | — |
| `/quit` | nothing: closes the connection and exits | — |

Output prefixes: `<<` received frame, `>>` sent line (echoed only when stdin is not a terminal),
`--` message from cli itself. When the server closes the connection, cli prints
`-- server closed the connection` and exits.

Input can be piped, for scripted checks. At the end of the input cli half-closes the socket
(`shutdown(SHUT_WR)`): the server sees `recv() = 0` after the last request, its replies are still
printed, and cli exits when the server closes its side.

```bash
printf 'PING\n/bytes PING\n/burst PING ;; PING ;; PING\n' | ./build/cli 127.0.0.1 5500
printf '/zero\n' | ./build/cli 127.0.0.1 5500      # expect: -- server closed the connection
```

Milestone M0: run `./build/cli 127.0.0.1 5500` in three terminals at once, `PING` in each, then
`/bytes PING` and `/burst PING ;; PING ;; PING` — every PING gets exactly one `<< 200|PING`.

## Running over a LAN

The server runs on Hân's Linux machine; clients run on other machines in the same network.
The server listens on `INADDR_ANY` (every interface of the machine), so nothing in the server
has to change: the clients only need its LAN IP address and an open port 5500.

1. **Start the server** on the server machine:

   ```bash
   ./build/caro_server 5500
   ```

2. **Find the server's IP address** (the `inet` address of the Wi-Fi or Ethernet interface,
   e.g. `192.168.1.23`; not `127.0.0.1`):

   ```bash
   ip -4 addr                # Ubuntu
   ipconfig getifaddr en0    # macOS (en0 = Wi-Fi on most Macs)
   ```

3. **Open port 5500** if the firewall is on. On Ubuntu, `sudo ufw status` shows it; when it says
   `active`:

   ```bash
   sudo ufw allow 5500/tcp
   ```

4. **Server inside WSL2** (Windows): the other machines must reach WSL through Windows. Use one
   of these, then give the clients the **Windows** machine's IP (`ipconfig` in Windows, "IPv4
   Address" of the Wi-Fi adapter).
   - **Mirrored networking** (Windows 11 22H2 or later): put this in `%UserProfile%\.wslconfig`,
     then run `wsl --shutdown` in PowerShell and open WSL again. WSL then uses the Windows
     network interfaces directly.

     ```ini
     [wsl2]
     networkingMode=mirrored
     ```

     If connections are still refused, allow the port in the Hyper-V firewall too (PowerShell
     as administrator):

     ```powershell
     New-NetFirewallHyperVRule -Name Caro5500 -DisplayName "Caro 5500" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol TCP -LocalPorts 5500
     ```

   - **Port forwarding** (default NAT networking): in WSL, `hostname -I` gives WSL's own address
     (e.g. `172.20.5.9`). In PowerShell as administrator, forward the port and open it in
     Windows Firewall. WSL's address changes after a restart: run `netsh` again then.

     ```powershell
     netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=5500 connectaddress=172.20.5.9 connectport=5500
     New-NetFirewallRule -DisplayName "Caro 5500" -Direction Inbound -Protocol TCP -LocalPort 5500 -Action Allow
     ```

5. **Check the network before opening the GUI**, from each client machine (replace the IP):

   ```bash
   ./build/cli 192.168.1.23 5500
   ```

   Type `PING`: the answer must be `<< 200|PING`. If `cli` cannot connect, the problem is the
   network or a firewall, not the game.

6. **Start the GUI** on each client machine and type the server's IP in the Server field
   (port `5500`).

**Campus Wi-Fi** often blocks traffic between devices of the same network ("client isolation"):
`cli` then hangs on connecting and fails after a minute or more, although both machines are
online. Use a phone hotspot instead: connect
the server and every client to it, find the server's IP again (step 2), and repeat step 5.

Notes from the LAN test (25/11/2026):

- **Server in VirtualBox**: set the VM's network adapter to **Bridged Adapter**. With NAT, other
  machines cannot see the VM.
- **macOS, "No route to host"**: macOS can block VS Code's terminal (Local Network permission).
  Run from Terminal.app, or allow VS Code in System Settings > Privacy & Security > Local Network.
- **iPhone hotspot**: addresses are `172.20.10.x`. Find them with `ip -4 addr` (Ubuntu) or
  `ipconfig getifaddr en0` (macOS).

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
