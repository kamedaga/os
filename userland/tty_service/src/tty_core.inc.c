enum {
    TTY_ATTR_VERSION = 2,
};

struct tty_attr_payload {
    u32 version;
    u32 iflag;
    u32 oflag;
    u32 lflag;
    u8 cc[TTY_CC_COUNT];
    u16 columns;
    u16 rows;
    u32 reserved0;
};

static struct bsd_tty g_tty;
static u8 g_tty_pump_buf[CONSOLE_RESPONSE_PAYLOAD_BYTES];

static void tty_core_init_defaults(void) {
    bsd_ttydisc_init_defaults(&g_tty);
}

static void tty_core_reset_session(void) {
    bsd_ttydisc_reset_session(&g_tty);
}

static void tty_core_notify_client_event(void) {
    if (g_client.reply_endpoint_id != 0) (void)signal_endpoint(g_client.reply_endpoint_id);
}

static u64 tty_core_pump_input_once(void) {
    const u64 n = backend_try_read_raw();
    if (n == 0) return 0;
    volatile u8 *src = payload_at(TTY_CONSOLE_RESPONSE_VA, CONSOLE_RESPONSE_HEADER_BYTES);
    for (u64 i = 0; i < n; i++) g_tty_pump_buf[i] = src[i];
    bsd_ttydisc_rint_bypass(&g_tty, g_tty_pump_buf, n);
    if (bsd_ttydisc_peek_signal(&g_tty) != 0) tty_core_notify_client_event();
    return n;
}

static void tty_core_pump_input(u64 budget) {
    for (u64 i = 0; i < budget; i++) {
        if (tty_core_pump_input_once() == 0) return;
    }
}

static u64 tty_read_to_client_payload(u64 max_len, u8 *signal_out) {
    const u32 request_len = (u32)min_u64(max_len, CONSOLE_RESPONSE_PAYLOAD_BYTES);
    if (request_len == 0) return 0;
    *signal_out = 0;
    for (;;) {
        const u8 signo = bsd_ttydisc_take_signal(&g_tty);
        if (signo != 0) {
            *signal_out = signo;
            return 0;
        }
        if (bsd_ttydisc_readable(&g_tty) ||
            bsd_ttyinq_bytes_used(&g_tty.inq) >= bsd_ttyinq_capacity()) {
            volatile u8 *dst = payload_at(TTY_CLIENT_RESPONSE_VA, CONSOLE_RESPONSE_HEADER_BYTES);
            return bsd_ttydisc_read(&g_tty, dst, request_len);
        }

        if (tty_core_pump_input_once() == 0) (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    }
}

static u8 tty_take_signal(void) {
    return bsd_ttydisc_take_signal(&g_tty);
}

static u64 tty_write_from_client_payload(volatile u8 *payload, u64 len) {
    return bsd_ttydisc_write(&g_tty, payload, len);
}

static u32 tty_export_attr(volatile u8 *dst, u64 max_len) {
    if (max_len < sizeof(struct tty_attr_payload)) return 0;
    struct tty_attr_payload attr;
    attr.version = TTY_ATTR_VERSION;
    attr.iflag = g_tty.termios.iflag;
    attr.oflag = g_tty.termios.oflag;
    attr.lflag = g_tty.termios.lflag;
    for (u64 i = 0; i < sizeof(attr.cc); i++) attr.cc[i] = g_tty.termios.cc[i];
    attr.columns = g_tty.termios.columns;
    attr.rows = g_tty.termios.rows;
    attr.reserved0 = 0;
    const u8 *src = (const u8 *)&attr;
    for (u64 i = 0; i < sizeof(attr); i++) dst[i] = src[i];
    return (u32)sizeof(attr);
}

static int tty_import_attr(volatile u8 *src, u64 len) {
    if (len < sizeof(struct tty_attr_payload)) return 1;
    struct tty_attr_payload attr;
    u8 *dst = (u8 *)&attr;
    for (u64 i = 0; i < sizeof(attr); i++) dst[i] = src[i];
    if (attr.version != TTY_ATTR_VERSION) return 0;
    g_tty.termios.iflag = attr.iflag;
    g_tty.termios.oflag = attr.oflag;
    g_tty.termios.lflag = attr.lflag;
    for (u64 i = 0; i < sizeof(g_tty.termios.cc); i++) g_tty.termios.cc[i] = attr.cc[i];
    if (attr.columns != 0) g_tty.termios.columns = attr.columns;
    if (attr.rows != 0) g_tty.termios.rows = attr.rows;
    return 1;
}

static u64 tty_columns(void) { return g_tty.termios.columns; }
static u64 tty_rows(void) { return g_tty.termios.rows; }
