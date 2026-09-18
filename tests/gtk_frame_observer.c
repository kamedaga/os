/* SPDX-License-Identifier: MIT */
/* GTK_MODULES observer. Uses public signals; never changes frame scheduling.
 * Build against GLib headers. GDK's opaque public declarations below avoid
 * pulling drawing-backend headers into this observation-only module. */
#include <glib-object.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct _GdkFrameClock GdkFrameClock;
extern GType gdk_frame_clock_get_type(void);
extern GType gtk_widget_get_type(void);
extern void gtk_container_forall(gpointer container,
                                 void (*callback)(gpointer, gpointer), gpointer data);
extern gint64 gdk_frame_clock_get_frame_counter(GdkFrameClock *clock);

enum { BEFORE, UPDATE, LAYOUT, PAINT, AFTER_BEGIN, AFTER_END, PHASE_COUNT };
static const char *const phases[] = {
    "before", "update", "layout", "paint", "after_begin", "after_end"
};
struct row { gint64 time, frame; unsigned clock, phase; };
static struct row rows[65536];
static GdkFrameClock *clocks[16];
static size_t used, dropped;
static gint64 start_time, end_time;
static unsigned duration_ms;
static gboolean active;
static GObject *fixed_fishbowl;
extern void frame_io_phase(int enabled);
extern void frame_io_window(gint64 end, int whole_thread);
extern void frame_io_dump(FILE *file);

static void record(GdkFrameClock *clock, unsigned phase);
static void after_paint(GdkFrameClock *clock, gpointer unused) {
    (void)unused;
    record(clock, AFTER_END);
}

static void record(GdkFrameClock *clock, unsigned phase) {
    unsigned index;
    for (index = 0; index < G_N_ELEMENTS(clocks); ++index) {
        if (clocks[index] == clock) break;
        if (!clocks[index]) {
            /* Bench windows stay open throughout the sample. Retain only
             * their clocks so a disposed object's address cannot be reused. */
            clocks[index] = g_object_ref(clock);
            g_signal_connect_after(clock, "after-paint", G_CALLBACK(after_paint), NULL);
            break;
        }
    }
    if (!active) return;
    if (phase == AFTER_BEGIN) frame_io_phase(0);
    gint64 now = g_get_monotonic_time();
    if (now >= end_time) return;
    if (phase == PAINT) frame_io_phase(1);
    if (index == G_N_ELEMENTS(clocks) || used == G_N_ELEMENTS(rows)) {
        ++dropped;
        return;
    }
    rows[used++] = (struct row){now, gdk_frame_clock_get_frame_counter(clock), index, phase};
}

static gboolean phase_hook(GSignalInvocationHint *hint, guint count,
                           const GValue *values, gpointer phase) {
    (void)hint;
    if (count) record(g_value_get_object(values), GPOINTER_TO_UINT(phase));
    return TRUE;
}

static gboolean finish(gpointer unused) {
    (void)unused;
    active = FALSE;
    frame_io_window(0, 0);
    const char *path = getenv("FRAME_OBSERVER_FILE");
    FILE *file = fopen(path ? path : "/tmp/frame-observer.csv", "w");
    if (!file) { perror("frame-observer"); return G_SOURCE_REMOVE; }
    char buffer[65536];
    setvbuf(file, buffer, _IOFBF, sizeof(buffer));
    fprintf(file, "# start_us=%" PRId64 " end_us=%" PRId64 " dropped=%zu\n",
            start_time, end_time, dropped);
    fputs("clock,frame,phase,monotonic_us\n", file);
    for (size_t i = 0; i < used; ++i)
        fprintf(file, "%u,%" PRId64 ",%s,%" PRId64 "\n",
                rows[i].clock, rows[i].frame, phases[rows[i].phase], rows[i].time);
    frame_io_dump(file);
    if (fclose(file)) perror("frame-observer close");
    fprintf(stderr, "FRAME_OBSERVER_DONE rows=%zu dropped=%zu start_us=%" PRId64
            " end_us=%" PRId64 "\n", used, dropped, start_time, end_time);
    return G_SOURCE_REMOVE;
}

static void collect_child(gpointer child, gpointer data) {
    GList **children = data;
    *children = g_list_prepend(*children, child);
}

static gboolean begin(gpointer unused) {
    (void)unused;
    if (getenv("FRAME_OBSERVER_FIXED_BUTTON")) {
        if (!fixed_fishbowl) abort();
        GList *children = NULL;
        /* Fishbowl exposes its animated widgets as internal children. */
        gtk_container_forall(fixed_fishbowl, collect_child, &children);
        gboolean valid = children && !children->next &&
            g_str_equal(G_OBJECT_TYPE_NAME(children->data), "GtkButton");
        fprintf(stderr, "FRAME_OBSERVER_CONTENT type=%s valid=%d\n",
                children ? G_OBJECT_TYPE_NAME(children->data) : "none", valid);
        g_list_free(children);
        if (!valid) abort();
    }
    start_time = g_get_monotonic_time();
    end_time = start_time + (gint64)duration_ms * 1000;
    active = TRUE;
    frame_io_window(end_time, getenv("FRAME_OBSERVER_ALL_IO") != NULL);
    g_timeout_add(duration_ms, finish, NULL);
    return G_SOURCE_REMOVE;
}

/* Equivalent to setting these existing demo properties in GTK Inspector.
 * Opt-in workload control, applied identically on both operating systems. */
static gboolean fixed_workload(GSignalInvocationHint *hint, guint count,
                               const GValue *values, gpointer unused) {
    (void)hint; (void)unused;
    if (count) {
        GObject *object = g_value_get_object(values);
        if (g_str_equal(G_OBJECT_TYPE_NAME(object), "GtkFishbowl")) {
            if (!fixed_fishbowl) fixed_fishbowl = g_object_ref(object);
            g_object_set(object, "benchmark", FALSE, "count", 1u, NULL);
            fprintf(stderr, "FRAME_OBSERVER_WORKLOAD benchmark=0 count=1\n");
        }
    }
    return TRUE;
}

void gtk_module_init(gint *argc, gchar ***argv) {
    (void)argc; (void)argv;
    const char *delay = getenv("FRAME_OBSERVER_DELAY_MS");
    const char *duration = getenv("FRAME_OBSERVER_DURATION_MS");
    unsigned delay_ms = delay ? (unsigned)strtoul(delay, NULL, 10) : 20000;
    duration_ms = duration ? (unsigned)strtoul(duration, NULL, 10) : 20000;
    if (delay_ms > 300000 || !duration_ms || duration_ms > 60000) abort();
    if (getenv("FRAME_OBSERVER_FIXED_COUNT")) {
        GType widget = gtk_widget_get_type();
        g_type_class_ref(widget);
        guint signal = g_signal_lookup("map", widget);
        if (!signal || !g_signal_add_emission_hook(signal, 0, fixed_workload, NULL, NULL))
            abort();
    }
    GType type = gdk_frame_clock_get_type();
    g_type_class_ref(type);
    const char *signals[] = {"before-paint", "update", "layout", "paint", "after-paint"};
    for (unsigned i = 0; i < G_N_ELEMENTS(signals); ++i) {
        guint signal = g_signal_lookup(signals[i], type);
        if (!signal || !g_signal_add_emission_hook(signal, 0, phase_hook, GUINT_TO_POINTER(i), NULL))
            abort();
    }
    g_timeout_add(delay_ms, begin, NULL);
    fprintf(stderr, "FRAME_OBSERVER_READY delay_ms=%u duration_ms=%u pid=%ld\n",
            delay_ms, duration_ms, (long)getpid());
}
