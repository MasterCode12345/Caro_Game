/*
 * client/ui_login.c — login / register page.
 * Owner: Nghĩa.
 *
 * SKELETON: no code yet. Stages (CLAUDE.md Roadmap):
 * TODO(C1): fields Server (default 127.0.0.1), Port (DEFAULT_PORT), Username, Password (hidden);
 *           buttons Login, Register; red error label.
 * TODO(C1): check username/password format (docs/protocol.md §2) before sending; connect
 *           first if not connected; lock the buttons while a response is pending.
 * TODO(C1): Vietnamese messages: 401 "Sai tên đăng nhập hoặc mật khẩu", 409 "Tài khoản đang
 *           đăng nhập ở nơi khác" / "Tên đã tồn tại", 400 -> the specific reason.
 * TODO(C1): 201|REGISTER -> "Đăng ký thành công, hãy đăng nhập"; 200|LOGIN -> store the user
 *           in app, show the lobby page.
 * TODO(C4): show the "lost connection" message when sent back here by net_on_lost().
 */
