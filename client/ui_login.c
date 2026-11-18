/*
 * client/ui_login.c — page "login": connect, REGISTER, LOGIN (docs/protocol.md §2, §6.1).
 * Owner: Nghĩa.
 *
 * Flow of a click: check the format here first (no point sending what the server must refuse,
 * and a '|' in a field would break the message) -> connect if needed -> lock the form ->
 * net_request() -> the callback unlocks the form and shows the result. The form stays locked
 * while a response is pending, so a double click cannot send the request twice.
 */
#include <string.h>

#include <gtk/gtk.h>

#include "app.h"
#include "dispatch.h"
#include "net.h"

static GtkWidget *e_host, *e_port, *e_user, *e_pass;
static GtkWidget *b_login, *b_register;
static GtkWidget *l_msg;

typedef enum { MSG_ERROR, MSG_OK, MSG_BUSY } MsgKind;

static void show_message(MsgKind kind, const char *text)
{
    if (text == NULL) {
        gtk_label_set_text(GTK_LABEL(l_msg), "");
        return;
    }
    const char *color = kind == MSG_ERROR ? "#c01c28" : kind == MSG_OK ? "#26a269" : "#5e5c64";
    char *markup = g_markup_printf_escaped("<span foreground='%s'>%s</span>", color, text);
    gtk_label_set_markup(GTK_LABEL(l_msg), markup);
    g_free(markup);
}

static void set_busy(gboolean busy)
{
    GtkWidget *w[] = { e_host, e_port, e_user, e_pass, b_login, b_register };
    for (size_t i = 0; i < G_N_ELEMENTS(w); i++)
        gtk_widget_set_sensitive(w[i], !busy);
}

/* ---- format checks (docs/protocol.md §2) ------------------------------------------------------- */

static const char *check_username(const char *u)
{
    size_t n = strlen(u);
    gboolean ok = n >= USERNAME_MIN && n <= USERNAME_MAX;
    for (const char *p = u; ok && *p != '\0'; p++)
        ok = g_ascii_isalnum(*p) || *p == '_';
    return ok ? NULL : "Tên đăng nhập phải dài 3–20 ký tự, chỉ gồm chữ không dấu, số và dấu _";
}

static const char *check_password(const char *pw)
{
    size_t n = strlen(pw);
    gboolean ok = n >= PASSWORD_MIN && n <= PASSWORD_MAX;
    for (const char *p = pw; ok && *p != '\0'; p++)
        ok = *p >= 0x21 && *p <= 0x7E && *p != '|';   /* printable ASCII, no space, no '|' */
    return ok ? NULL : "Mật khẩu phải dài 6–32 ký tự ASCII, không dấu cách, không dấu |";
}

/* ---- responses ---------------------------------------------------------------------------------- */

static const char *reason_text(int code, const char *reason)
{
    static const struct { const char *token, *text; } table[] = {
        { ERR_INVALID_CREDENTIALS, "Sai tên đăng nhập hoặc mật khẩu" },
        { ERR_ALREADY_LOGGED_IN,   "Tài khoản đang đăng nhập ở nơi khác" },
        { ERR_USERNAME_TAKEN,      "Tên đã tồn tại" },
        { ERR_INVALID_USERNAME,    "Tên đăng nhập không hợp lệ (3–20 ký tự: chữ không dấu, số, _)" },
        { ERR_INVALID_PASSWORD,    "Mật khẩu không hợp lệ (6–32 ký tự ASCII, không dấu cách, không |)" },
        { ERR_BAD_ARGS,            "Yêu cầu sai định dạng" },
        { ERR_FORBIDDEN_STATE,     "Không thể thực hiện ở trạng thái hiện tại" },
        { ERR_SERVER_ERROR,        "Lỗi phía server, hãy thử lại sau" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(table); i++)
        if (strcmp(reason, table[i].token) == 0)
            return table[i].text;
    if (code == ST_NOT_AUTHENTICATED)
        return "Sai tên đăng nhập hoặc mật khẩu";
    if (code == ST_SERVER_ERROR)
        return "Lỗi phía server, hãy thử lại sau";
    return NULL;
}

static void show_error_response(int code, char **f, int n)
{
    const char *reason = n >= 3 ? f[2] : "";
    const char *text = reason_text(code, reason);
    if (text != NULL) {
        show_message(MSG_ERROR, text);
    } else {
        char *s = g_strdup_printf("Lỗi %d: %s", code, *reason != '\0' ? reason : "không rõ lý do");
        show_message(MSG_ERROR, s);
        g_free(s);
    }
}

static void on_register_reply(int code, char **f, int n, gpointer data)
{
    (void)data;
    set_busy(FALSE);
    if (code == RESP_DISCONNECTED)
        return;                       /* net_on_lost() shows the message */
    if (code == ST_CREATED)
        show_message(MSG_OK, "Đăng ký thành công, hãy đăng nhập");
    else
        show_error_response(code, f, n);
}

static void on_login_reply(int code, char **f, int n, gpointer data)
{
    (void)data;
    set_busy(FALSE);
    if (code == RESP_DISCONNECTED)
        return;
    if (code != ST_OK) {
        show_error_response(code, f, n);
        return;
    }

    /* 200|LOGIN|user|elo|wins|losses|draws */
    int elo, wins, losses, draws;
    if (n < 7 || !field_int(f[3], &elo) || !field_int(f[4], &wins) || !field_int(f[5], &losses)
        || !field_int(f[6], &draws) || check_username(f[2]) != NULL) {
        show_message(MSG_ERROR, "Server trả về phản hồi LOGIN không hợp lệ");
        net_close();                  /* do not stay half logged in */
        return;
    }
    snprintf(app.username, sizeof app.username, "%s", f[2]);
    app.elo = elo;
    app.wins = wins;
    app.losses = losses;
    app.draws = draws;
    app.state = SESSION_LOBBY;

    gtk_entry_set_text(GTK_ENTRY(e_pass), "");
    show_message(MSG_OK, NULL);
    app_show_page("lobby");
}

/* ---- submit ------------------------------------------------------------------------------------- */

static void submit(gboolean is_register)
{
    char *host = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e_host))));
    const char *port_s = gtk_entry_get_text(GTK_ENTRY(e_port));
    const char *user = gtk_entry_get_text(GTK_ENTRY(e_user));
    const char *pass = gtk_entry_get_text(GTK_ENTRY(e_pass));
    int port;
    const char *problem = NULL;

    if (*host == '\0')
        problem = "Hãy nhập địa chỉ server";
    else if (!field_int(port_s, &port) || port < 1 || port > 65535)
        problem = "Cổng phải là số từ 1 đến 65535";
    else if ((problem = check_username(user)) == NULL)
        problem = check_password(pass);
    if (problem != NULL) {
        show_message(MSG_ERROR, problem);
        g_free(host);
        return;
    }

    /* Connect only when needed: a failed LOGIN keeps the connection for the next try. A changed
     * Server/Port field means another server: net_connect() closes the old connection. */
    if (!net_is_connected_to(host, port)) {
        char *err = NULL;
        if (!net_connect(host, port, &err)) {
            char *s = g_strdup_printf("Không kết nối được tới %s:%d (%s)", host, port, err);
            show_message(MSG_ERROR, s);
            g_free(s);
            g_free(err);
            g_free(host);
            return;
        }
    }
    g_free(host);

    char *payload = g_strdup_printf("%s|%s|%s", is_register ? CMD_REGISTER : CMD_LOGIN, user, pass);
    set_busy(TRUE);
    show_message(MSG_BUSY, is_register ? "Đang đăng ký…" : "Đang đăng nhập…");
    if (!net_request(payload, is_register ? on_register_reply : on_login_reply, NULL)) {
        set_busy(FALSE);
        show_message(MSG_ERROR, "Không gửi được yêu cầu tới server");
    }
    g_free(payload);
}

static void on_login_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    submit(FALSE);
}

static void on_register_clicked(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    submit(TRUE);
}

/* ---- page --------------------------------------------------------------------------------------- */

void ui_login_reset(const char *error)
{
    set_busy(FALSE);
    show_message(MSG_ERROR, error);
    gtk_widget_grab_focus(*gtk_entry_get_text(GTK_ENTRY(e_user)) != '\0' ? e_pass : e_user);
}

static GtkWidget *grid_row(GtkWidget *grid, int row, const char *label_text, GtkWidget *entry)
{
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_END);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
    g_signal_connect(entry, "activate", G_CALLBACK(on_login_clicked), NULL);  /* Enter = login */
    return entry;
}

GtkWidget *ui_login_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(box, 360, -1);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='xx-large' weight='bold'>Caro Online</span>");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("Nhóm 13 · Lập trình mạng"), FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    e_host = grid_row(grid, 0, "Server", gtk_entry_new());
    e_port = grid_row(grid, 1, "Cổng", gtk_entry_new());
    e_user = grid_row(grid, 2, "Tên đăng nhập", gtk_entry_new());
    e_pass = grid_row(grid, 3, "Mật khẩu", gtk_entry_new());
    gtk_entry_set_text(GTK_ENTRY(e_host), "127.0.0.1");
    char port_s[8];
    snprintf(port_s, sizeof port_s, "%d", DEFAULT_PORT);
    gtk_entry_set_text(GTK_ENTRY(e_port), port_s);
    gtk_entry_set_max_length(GTK_ENTRY(e_port), 5);
    gtk_entry_set_max_length(GTK_ENTRY(e_user), USERNAME_MAX);
    gtk_entry_set_max_length(GTK_ENTRY(e_pass), PASSWORD_MAX);
    gtk_entry_set_visibility(GTK_ENTRY(e_pass), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(e_pass), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 8);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    b_login = gtk_button_new_with_label("Đăng nhập");
    b_register = gtk_button_new_with_label("Đăng ký");
    gtk_style_context_add_class(gtk_widget_get_style_context(b_login), "suggested-action");
    g_signal_connect(b_login, "clicked", G_CALLBACK(on_login_clicked), NULL);
    g_signal_connect(b_register, "clicked", G_CALLBACK(on_register_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), b_login, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), b_register, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    l_msg = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(l_msg), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l_msg), 45);
    gtk_label_set_justify(GTK_LABEL(l_msg), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(box), l_msg, FALSE, FALSE, 0);

    if (app.fake_mode) {
        GtkWidget *hint = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hint),
            "<small>Chế độ offline (CARO_FAKE=1). Thử lỗi: mật khẩu <b>wrong</b> → 401, "
            "đăng ký tên <b>taken</b> → 409, đăng nhập tên <b>online</b> → 409.</small>");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(hint), 45);
        gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
        gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 12);
    }
    return box;
}
