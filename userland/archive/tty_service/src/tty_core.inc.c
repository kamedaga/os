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

static int tty_core_read_ready(void);
static u64 tty_core_input_used(void);

static void tty_core_refresh_backend_winsize(void) {
    u16 columns = 0;
    u16 rows = 0;
    if (!backend_get_winsize(&columns, &rows)) return;
    g_tty.termios.columns = columns;
    g_tty.termios.rows = rows;
}

static void tty_core_init_defaults(void) {
    bsd_ttydisc_init_defaults(&g_tty);
    tty_core_refresh_backend_winsize();
}

static void tty_core_reset_session(void) {
    bsd_ttydisc_reset_session(&g_tty);
}

static void tty_core_notify_client_event(void) {
    if (g_client.reply_endpoint_id != 0) {
        (void)syscall2(SYSCALL_SIGNAL_ENDPOINT, g_client.reply_endpoint_id, 0);
    }
}

static void tty_core_update_readable_notification(void) {
    const int readable = bsd_ttydisc_peek_signal(&g_tty) != 0 || tty_core_read_ready();
    if (!readable) {
        return;
    }
    tty_core_notify_client_event();
}

static u64 tty_core_pump_input_once(void) {
    const u64 n = backend_try_read_raw();
    if (n == 0) return 0;
    volatile u8 *src = payload_at(g_console.response_va, CONSOLE_RESPONSE_HEADER_BYTES);
    for (u64 i = 0; i < n; i++) g_tty_pump_buf[i] = src[i];
    bsd_ttydisc_rint_bypass(&g_tty, g_tty_pump_buf, n);
    tty_core_update_readable_notification();
    return n;
}

static u64 tty_core_pump_input(u64 budget) {
    u64 total = 0;
    for (u64 i = 0; i < budget; i++) {
        const u64 n = tty_core_pump_input_once();
        if (n == 0) break;
        total += n;
    }
    if (total != 0) (void)syscall2(SYSCALL_WAIT_EVENT, 0, 1);
    return total;
}

static u64 tty_core_input_used(void) {
    return bsd_ttyinq_bytes_used(&g_tty.inq);
}

static int tty_core_is_canonical(void) {
    return (g_tty.termios.lflag & TTY_LFLAG_ICANON) != 0;
}

static u64 tty_core_vmin(void) {
    return g_tty.termios.cc[TTY_CC_VMIN];
}

static u64 tty_core_vtime_ticks(void) {
    return (u64)g_tty.termios.cc[TTY_CC_VTIME] * 100ULL;
}

enum tty_read_decision {
    TTY_READ_WAIT = 0,
    TTY_READ_DATA = 1,
    TTY_READ_ZERO = 2,
};

static int tty_core_read_ready(void) {
    if (tty_core_is_canonical()) {
        return bsd_ttydisc_readable(&g_tty) ||
            bsd_ttyinq_bytes_used(&g_tty.inq) >= bsd_ttyinq_capacity();
    }
    const u64 used = tty_core_input_used();
    const u64 vmin = tty_core_vmin();
    if (vmin == 0) return used != 0;
    if (tty_core_vtime_ticks() != 0) return used != 0;
    return used >= vmin;
}

static enum tty_read_decision tty_core_read_decision(u64 request_len, u64 *timer_start_tick) {
    if (tty_core_is_canonical()) {
        return (bsd_ttydisc_readable(&g_tty) ||
            bsd_ttyinq_bytes_used(&g_tty.inq) >= bsd_ttyinq_capacity()) ? TTY_READ_DATA : TTY_READ_WAIT;
    }

    const u64 used = tty_core_input_used();
    const u64 vmin = tty_core_vmin();
    const u64 vtime_ticks = tty_core_vtime_ticks();
    if (vmin == 0) {
        if (used != 0) {
            *timer_start_tick = 0;
            return TTY_READ_DATA;
        }
        if (vtime_ticks == 0) return TTY_READ_ZERO;

        const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
        if (*timer_start_tick == 0) {
            *timer_start_tick = now;
            return TTY_READ_WAIT;
        }
        return now - *timer_start_tick >= vtime_ticks ? TTY_READ_ZERO : TTY_READ_WAIT;
    }

    const u64 needed = min_u64(vmin, request_len);
    if (used >= needed) {
        *timer_start_tick = 0;
        return TTY_READ_DATA;
    }
    if (vtime_ticks == 0 || used == 0) {
        *timer_start_tick = 0;
        return TTY_READ_WAIT;
    }

    const u64 now = syscall0(SYSCALL_GET_TICK_COUNT);
    if (*timer_start_tick == 0) {
        *timer_start_tick = now;
        return TTY_READ_WAIT;
    }
    return now - *timer_start_tick >= vtime_ticks ? TTY_READ_DATA : TTY_READ_WAIT;
}

static u64 tty_core_read_copy_len(u64 request_len) {
    if (tty_core_is_canonical()) return request_len;
    u64 cap = TTY_RAW_READ_CHUNK_BYTES;
    const u64 vmin = tty_core_vmin();
    if (vmin > cap) cap = vmin;
    return min_u64(request_len, cap);
}

static u64 tty_try_read_to_client_payload(u64 max_len, u64 *timer_start_tick, u8 *signal_out, int *again_out) {
    const u32 request_len = (u32)min_u64(max_len, CONSOLE_RESPONSE_PAYLOAD_BYTES);
    *signal_out = 0;
    *again_out = 0;
    if (request_len == 0) return 0;

    const u8 signo = bsd_ttydisc_take_signal(&g_tty);
    if (signo != 0) {
        *signal_out = signo;
        return 0;
    }
    tty_core_pump_input(TTY_PUMP_READ_BUDGET);
    const enum tty_read_decision decision = tty_core_read_decision(request_len, timer_start_tick);
    if (decision == TTY_READ_WAIT) {
        *again_out = 1;
        return 0;
    }
    if (decision == TTY_READ_ZERO) return 0;
    volatile u8 *dst = payload_at(g_client.response_va, CONSOLE_RESPONSE_HEADER_BYTES);
    const u64 n = bsd_ttydisc_read(&g_tty, dst, tty_core_read_copy_len(request_len));
    return n;
}

static u8 tty_take_signal(void) {
    return bsd_ttydisc_take_signal(&g_tty);
}

static int tty_client_readable(void) {
    tty_core_pump_input(TTY_PUMP_READ_BUDGET);
    if (bsd_ttydisc_peek_signal(&g_tty) != 0) return 1;
    return tty_core_read_ready();
}

static u64 tty_write_from_client_payload(volatile u8 *payload, u64 len) {
    return bsd_ttydisc_write(&g_tty, payload, len);
}

static u32 tty_export_attr(volatile u8 *dst, u64 max_len) {
    if (max_len < sizeof(struct tty_attr_payload)) return 0;
    tty_core_refresh_backend_winsize();
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
    tty_core_update_readable_notification();
    return 1;
}

static u64 tty_columns(void) { return g_tty.termios.columns; }
static u64 tty_rows(void) { return g_tty.termios.rows; }
