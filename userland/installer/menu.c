/* SPDX-License-Identifier: MIT */
#include "menu.h"
#include "fat32_probe.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

enum {
    UI_MIN_ROWS = 20,
    UI_MIN_COLUMNS = 60,
    UI_MAX_COLUMNS = 100,
    UI_ESCAPE_TIMEOUT_NS = 50000000,
    UI_KEY_ENTER = 0x100,
    UI_KEY_ESCAPE,
    UI_KEY_UP,
    UI_KEY_DOWN,
    UI_KEY_LEFT,
    UI_KEY_RIGHT,
};

struct ui_screen { FILE *output; unsigned rows; unsigned columns; };
struct ui_box { unsigned top; unsigned left; unsigned height; unsigned width; };

struct ui_progress {
    const struct ui_screen *screen;
    struct ui_box box;
    unsigned percent;
    char phase[160];
};

static unsigned bounded(unsigned value, unsigned low, unsigned high) {
    if (value < low) return low;
    return value > high ? high : value;
}

static struct ui_screen make_screen(FILE *output, unsigned rows,
    unsigned columns) {
    return (struct ui_screen){.output = output,
        .rows = bounded(rows, UI_MIN_ROWS, 200),
        .columns = bounded(columns, UI_MIN_COLUMNS, 300)};
}

static void move_to(const struct ui_screen *s, unsigned row, unsigned column) {
    fprintf(s->output, "\033[%u;%uH", row, column);
}

static void screen_begin(const struct ui_screen *s) {
    /* The installer owns this PTY while its menu is active. Alternate-screen
     * mode keeps boot messages in normal scrollback while menu redraws do not
     * look like another group of log lines. */
    fputs("\033[?1049h\033[2J\033[H\033[?25l", s->output);
}

static void screen_end(const struct ui_screen *s) {
    fputs("\033[0m\033[?25h", s->output);
    fflush(s->output);
}

static void clear_screen(const struct ui_screen *s) {
    fputs("\033[0m\033[2J\033[H\033[?25l", s->output);
}

static struct ui_box centered_box(const struct ui_screen *s,
    unsigned wanted_height, unsigned wanted_width) {
    const unsigned height = bounded(wanted_height, 8, s->rows - 2);
    const unsigned max_width = s->columns > 4 ? s->columns - 4 : 1;
    const unsigned width = bounded(wanted_width, 56,
        max_width < UI_MAX_COLUMNS ? max_width : UI_MAX_COLUMNS);
    return (struct ui_box){.top = 1 + (s->rows - height) / 2,
        .left = 1 + (s->columns - width) / 2,
        .height = height, .width = width};
}

static void repeat(FILE *output, char c, unsigned count) {
    while (count--) fputc(c, output);
}

static void draw_box(const struct ui_screen *s, struct ui_box box,
    const char *title) {
    move_to(s, box.top, box.left);
    fputc('+', s->output); repeat(s->output, '-', box.width - 2);
    fputc('+', s->output);
    for (unsigned row = 1; row + 1 < box.height; ++row) {
        move_to(s, box.top + row, box.left);
        fputc('|', s->output); repeat(s->output, ' ', box.width - 2);
        fputc('|', s->output);
    }
    move_to(s, box.top + box.height - 1, box.left);
    fputc('+', s->output); repeat(s->output, '-', box.width - 2);
    fputc('+', s->output);
    if (title && *title) {
        const size_t length = strlen(title);
        unsigned column = box.left + 2;
        if (length + 4 < box.width)
            column = box.left + (box.width - (unsigned)length) / 2;
        move_to(s, box.top, column - 1);
        fprintf(s->output, "[ %s ]", title);
    }
}

static void clear_box_row(const struct ui_screen *s, struct ui_box box,
    unsigned row) {
    move_to(s, box.top + row, box.left + 1);
    repeat(s->output, ' ', box.width - 2);
    move_to(s, box.top + row, box.left + box.width - 1);
    fputc('|', s->output);
}

static void box_text(const struct ui_screen *s, struct ui_box box,
    unsigned row, const char *text) {
    const size_t length = strlen(text);
    const unsigned inner = box.width - 4;
    const unsigned shown = length > inner ? inner : (unsigned)length;
    move_to(s, box.top + row, box.left + 2 + (inner - shown) / 2);
    fwrite(text, 1, shown, s->output);
}

static void write_box_inner_row(const struct ui_screen *s,
    struct ui_box box, unsigned row, const char *contents,
    size_t contents_length) {
    if (!s || !s->output || box.width < 2 || row >= box.height)
        return;
    const unsigned inner = box.width - 2;
    char packet[32 + UI_MAX_COLUMNS];
    const int prefix = snprintf(packet, sizeof(packet), "\033[%u;%uH",
        box.top + row, box.left + 1);
    if (prefix < 0 || (size_t)prefix + inner > sizeof(packet))
        return;
    memset(packet + prefix, ' ', inner);
    if (contents && contents_length) {
        if (contents_length > inner) contents_length = inner;
        const unsigned offset = (inner - (unsigned)contents_length) / 2;
        memcpy(packet + prefix + offset, contents, contents_length);
    }
    /* Keep the cursor move, erasure and replacement text in one stream write.
     * termd may render between separate stdio calls; emitting a partially
     * cleared progress row made the left side of the bar disappear on the
     * real framebuffer console. */
    fwrite(packet, 1, (size_t)prefix + inner, s->output);
}

static void box_line(const struct ui_screen *s, struct ui_box box,
    unsigned row, const char *format, ...) {
    char line[256];
    va_list args;
    va_start(args, format); vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    const size_t length = strlen(line), maximum = box.width - 4;
    move_to(s, box.top + row, box.left + 2);
    fwrite(line, 1, length > maximum ? maximum : length, s->output);
}

static void draw_install_progress(void *context, unsigned percent,
    const char *phase) {
    struct ui_progress *progress = context;
    if (!progress || !progress->screen) return;
    if (percent > 100) percent = 100;
    if (percent < progress->percent) percent = progress->percent;
    if (phase && *phase)
        snprintf(progress->phase, sizeof(progress->phase), "%s", phase);
    progress->percent = percent;
    const struct ui_screen *s = progress->screen;
    const struct ui_box box = progress->box;
    const char *label = progress->phase[0] ? progress->phase : "Preparing";
    write_box_inner_row(s, box, 3, label, strlen(label));

    const unsigned inner = box.width - 2;
    const unsigned maximum = inner > 11 ? inner - 11 : 1;
    const unsigned filled = maximum * percent / 100;
    char bar[UI_MAX_COLUMNS + 1];
    size_t cursor = 0;
    bar[cursor++] = '[';
    memset(bar + cursor, '#', filled);
    cursor += filled;
    memset(bar + cursor, '-', maximum - filled);
    cursor += maximum - filled;
    const int suffix = snprintf(bar + cursor, sizeof(bar) - cursor,
        "] %3u%%", percent);
    if (suffix > 0) cursor += (size_t)suffix;
    write_box_inner_row(s, box, 5, bar, cursor);
    fflush(s->output);
}

static int read_event(installer_read_key read_key, void *context) {
    int key = read_key(context, UINT64_MAX);
    if (key < 0) return key;
    if (key == '\r' || key == '\n') return UI_KEY_ENTER;
    if (key != 0x1b) return key;
    int second = read_key(context, UI_ESCAPE_TIMEOUT_NS);
    if (second == -ETIMEDOUT) return UI_KEY_ESCAPE;
    if (second < 0 || second != '[') return UI_KEY_ESCAPE;
    int third = read_key(context, UI_ESCAPE_TIMEOUT_NS);
    if (third == -ETIMEDOUT) return UI_KEY_ESCAPE;
    if (third < 0) return third;
    switch (third) {
    case 'A': return UI_KEY_UP;
    case 'B': return UI_KEY_DOWN;
    case 'C': return UI_KEY_RIGHT;
    case 'D': return UI_KEY_LEFT;
    default: return UI_KEY_ESCAPE;
    }
}

static void draw_two_choices(const struct ui_screen *s, struct ui_box box,
    unsigned row, const char *left, const char *right, unsigned selected) {
    char left_text[64], right_text[64];
    snprintf(left_text, sizeof(left_text),
        selected == 0 ? "[<%s>]" : " <%s> ", left);
    snprintf(right_text, sizeof(right_text),
        selected == 1 ? "[<%s>]" : " <%s> ", right);
    const unsigned gap = 8;
    const unsigned total = (unsigned)strlen(left_text) +
        (unsigned)strlen(right_text) + gap;
    unsigned column = box.left + (box.width - total) / 2;
    move_to(s, box.top + row, column);
    if (selected == 0) fputs("\033[30;47m", s->output);
    fputs(left_text, s->output);
    fputs("\033[0m", s->output);
    repeat(s->output, ' ', gap);
    if (selected == 1) fputs("\033[30;47m", s->output);
    fputs(right_text, s->output);
    fputs("\033[0m", s->output);
    fflush(s->output);
}

static int choose_two(installer_read_key read_key, void *context,
    const struct ui_screen *s, struct ui_box box, unsigned row,
    const char *left, const char *right, unsigned *selected,
    int left_hotkey, int right_hotkey) {
    for (;;) {
        draw_two_choices(s, box, row, left, right, *selected);
        int key = read_event(read_key, context);
        if (key < 0) return key;
        if (key == UI_KEY_ESCAPE) return -ECANCELED;
        if (key == UI_KEY_ENTER) return 0;
        if (key == UI_KEY_LEFT || key == UI_KEY_UP) *selected = 0;
        else if (key == UI_KEY_RIGHT || key == UI_KEY_DOWN ||
            key == '\t') *selected = 1;
        else if (key == left_hotkey || key == left_hotkey - 'a' + 'A') {
            *selected = 0; return 0;
        } else if (key == right_hotkey || key == right_hotkey - 'a' + 'A') {
            *selected = 1; return 0;
        }
    }
}

static int edit_line(installer_read_key read_key, void *context,
    const struct ui_screen *s, struct ui_box box, unsigned row,
    const char *prompt, char *line, size_t capacity) {
    size_t length = 0;
    line[0] = 0;
    for (;;) {
        clear_box_row(s, box, row);
        move_to(s, box.top + row, box.left + 2);
        fputs(prompt, s->output);
        const size_t maximum = box.width - 4 - strlen(prompt);
        fwrite(line, 1, length > maximum ? maximum : length, s->output);
        fputs("\033[?25h", s->output);
        fflush(s->output);
        int key = read_event(read_key, context);
        if (key < 0) return key;
        if (key == UI_KEY_ESCAPE) {
            fputs("\033[?25l", s->output);
            return -ECANCELED;
        }
        if (key == UI_KEY_ENTER) {
            fputs("\033[?25l", s->output);
            return 0;
        }
        if (key == 0x7f || key == '\b') {
            if (length) line[--length] = 0;
        } else if (key >= 0x20 && key <= 0x7e &&
            length + 1 < capacity && length < maximum) {
            line[length++] = (char)key;
            line[length] = 0;
        }
    }
}

static void disk_summary(char *buffer, size_t capacity, size_t number,
    const struct installer_disk *disk) {
    snprintf(buffer, capacity, "%zu  %-18.18s %7.1f GiB  %s",
        number, disk->model && *disk->model ? disk->model : "Unknown disk",
        (double)disk->bytes / (1024.0 * 1024.0 * 1024.0),
        disk->name ? disk->name : "unnamed");
}

static void identity_summary(char *buffer, size_t capacity,
    const struct installer_disk *disk) {
    snprintf(buffer, capacity,
        "ID %" PRIu64 "/%" PRIu64 "  dev %u:%u  diskseq %" PRIu64,
        disk->resource_id, disk->generation, disk->major, disk->minor,
        disk->diskseq);
}

static int valid_target(const struct installer_disk *disk) {
    return disk && disk->resource_id && disk->generation && disk->diskseq &&
        disk->major && disk->logical_block && disk->bytes &&
        (disk->gpt || disk->blank_verified) &&
        !(disk->gpt && disk->gpt->install_media);
}

static int select_disk(installer_read_key read_key, void *context,
    const struct ui_screen *s, const struct installer_disk *disks,
    size_t count, size_t media_index, size_t *target) {
    size_t candidates[32], candidate_count = 0;
    for (size_t i = 0; i < count && candidate_count < 32; ++i)
        if (i != media_index && valid_target(&disks[i]))
            candidates[candidate_count++] = i;
    if (!candidate_count) return -ENODEV;
    size_t selected = 0;
    for (;;) {
        clear_screen(s);
        struct ui_box box = centered_box(s, 18, 76);
        draw_box(s, box, "Select Installation Disk");
        box_text(s, box, 2,
            "The boot USB is identified and cannot be selected.");
        const size_t visible = (box.height - 7) / 2;
        size_t first = selected >= visible ? selected - visible + 1 : 0;
        if (first + visible > candidate_count && candidate_count > visible)
            first = candidate_count - visible;
        unsigned row = 4;
        for (size_t slot = 0; slot < visible &&
             first + slot < candidate_count; ++slot) {
            const size_t position = first + slot;
            const size_t index = candidates[position];
            char summary[160], identity[160];
            disk_summary(summary, sizeof(summary), index, &disks[index]);
            identity_summary(identity, sizeof(identity), &disks[index]);
            move_to(s, box.top + row, box.left + 2);
            if (position == selected) fputs("\033[30;47m", s->output);
            fprintf(s->output, "%c %-*.*s",
                position == selected ? '>' : ' ',
                (int)box.width - 6, (int)box.width - 6, summary);
            fputs("\033[0m", s->output);
            box_line(s, box, row + 1, "    %s", identity);
            row += 2;
        }
        box_text(s, box, box.height - 3,
            "Up/Down: select   Enter: continue   Esc: back");
        fflush(s->output);
        int key = read_event(read_key, context);
        if (key < 0) return key;
        if (key == UI_KEY_ESCAPE) return -ECANCELED;
        if (key == UI_KEY_ENTER) {
            *target = candidates[selected];
            return 0;
        }
        if (key == UI_KEY_UP)
            selected = selected ? selected - 1 : candidate_count - 1;
        else if (key == UI_KEY_DOWN)
            selected = (selected + 1) % candidate_count;
    }
}

static int select_partition(installer_read_key read_key, void *context,
    const struct ui_screen *s, struct ui_box box,
    const struct installer_gpt *gpt, const char *heading, size_t *selection) {
    if (!gpt || !gpt->partition_count) return -EINVAL;
    size_t selected = *selection < gpt->partition_count ? *selection : 0;
    for (;;) {
        for (unsigned row = 2; row + 4 < box.height; ++row)
            clear_box_row(s, box, row);
        box_text(s, box, 2, heading);
        const size_t visible = box.height - 8;
        size_t first = selected >= visible ? selected - visible + 1 : 0;
        unsigned row = 4;
        for (size_t slot = 0; slot < visible &&
             first + slot < gpt->partition_count; ++slot) {
            size_t index = first + slot;
            const struct installer_partition *p = &gpt->partitions[index];
            move_to(s, box.top + row++, box.left + 2);
            if (index == selected) fputs("\033[30;47m", s->output);
            fprintf(s->output, "%c %-*.*s",
                index == selected ? '>' : ' ',
                (int)box.width - 6, (int)box.width - 6, "");
            move_to(s, box.top + row - 1, box.left + 2);
            fprintf(s->output, "%c %zu  %-18.18s LBA %" PRIu64 "..%" PRIu64,
                index == selected ? '>' : ' ', index, p->name,
                p->first_lba, p->last_lba);
            fputs("\033[0m", s->output);
        }
        box_text(s, box, box.height - 3,
            "Up/Down: select   Enter: continue   Esc: back");
        fflush(s->output);
        int key = read_event(read_key, context);
        if (key < 0) return key;
        if (key == UI_KEY_ESCAPE) return -ECANCELED;
        if (key == UI_KEY_ENTER) { *selection = selected; return 0; }
        if (key == UI_KEY_UP)
            selected = selected ? selected - 1 : gpt->partition_count - 1;
        else if (key == UI_KEY_DOWN)
            selected = (selected + 1) % gpt->partition_count;
    }
}

static void extent_summary(char *buffer, size_t capacity, const char *name,
    struct installer_extent extent, uint32_t block) {
    snprintf(buffer, capacity, "%s: LBA %" PRIu64 "..%" PRIu64
        " (%.1f MiB)", name, extent.first_lba, extent.last_lba,
        (double)(extent.last_lba - extent.first_lba + 1) * block /
            (1024.0 * 1024.0));
}

void installer_ui_message(FILE *output, unsigned rows, unsigned columns,
    const char *title, const char *message, int error) {
    if (!output) return;
    const struct ui_screen s = make_screen(output, rows, columns);
    clear_screen(&s);
    const struct ui_box box = centered_box(&s, 9, 64);
    draw_box(&s, box, title ? title : "PachaOS Installer");
    if (error) fputs("\033[1;31m", output);
    box_text(&s, box, 3, message ? message : "");
    fputs("\033[0m", output);
    box_text(&s, box, 6, error ? "The live shell will remain available."
        : "You may now continue from the live shell.");
    screen_end(&s);
}

void installer_ui_leave_for_shell(FILE *output) {
    if (!output) return;
    fputs("\033[0m\033[2J\033[H\033[?25h", output);
    fflush(output);
}

int installer_ui_run(installer_read_key read_key, void *input_context,
    FILE *output, const struct installer_disk *disks, size_t count,
    size_t media_index, unsigned rows, unsigned columns,
    installer_execute execute, void *context) {
    if (!read_key || !output || !disks || !count || media_index >= count ||
        !disks[media_index].gpt || !disks[media_index].gpt->install_media ||
        !execute) return -EINVAL;
    size_t markers = 0;
    for (size_t i = 0; i < count; ++i)
        markers += disks[i].gpt && disks[i].gpt->install_media;
    if (markers != 1) return -EEXIST;

    enum { PAGE_WELCOME, PAGE_TARGET, PAGE_LAYOUT, PAGE_CONFIRM } page =
        PAGE_WELCOME;
    const struct ui_screen s = make_screen(output, rows, columns);
    unsigned welcome_choice = 0, layout_choice = 0;
    size_t target = 0;
    struct installer_plan plan = {0};
    int result = 0;
    screen_begin(&s);

    for (;;) {
        if (page == PAGE_WELCOME) {
            clear_screen(&s);
            struct ui_box box = centered_box(&s, 14, 68);
            draw_box(&s, box, "PachaOS Installer");
            box_text(&s, box, 3, "Welcome to PachaOS");
            box_text(&s, box, 5,
                "Install PachaOS to a local disk, or enter the live shell.");
            box_text(&s, box, 10,
                "Left/Right: select   Enter: continue");
            result = choose_two(read_key, input_context, &s, box, 8,
                "Install", "Shell", &welcome_choice, 'i', 's');
            if (result == -ECANCELED) {
                result = INSTALLER_UI_SHELL;
                break;
            }
            if (result) break;
            if (welcome_choice == 1) {
                result = INSTALLER_UI_SHELL;
                break;
            }
            page = PAGE_TARGET;
            continue;
        }

        if (page == PAGE_TARGET) {
            result = select_disk(read_key, input_context, &s, disks,
                count, media_index, &target);
            if (result == -ECANCELED) {
                result = 0; page = PAGE_WELCOME; continue;
            }
            if (result) break;
            page = PAGE_LAYOUT;
            continue;
        }

        const struct installer_disk *disk = &disks[target];
        if (page == PAGE_LAYOUT) {
            clear_screen(&s);
            struct ui_box box = centered_box(&s, 14, 72);
            draw_box(&s, box, "Disk Layout");
            char summary[160];
            disk_summary(summary, sizeof(summary), target, disk);
            box_text(&s, box, 3, summary);
            box_text(&s, box, 8,
                "Guided creates a FAT32 ESP and an ext4 root filesystem.");
            box_text(&s, box, 11,
                "Left/Right: select   Enter: continue   Esc: back");
            result = choose_two(read_key, input_context, &s, box, 6,
                "Guided UEFI/GPT", "Manual partitions", &layout_choice,
                'g', 'm');
            if (result == -ECANCELED) {
                result = 0; page = PAGE_TARGET; continue;
            }
            if (result) break;
            if (layout_choice == 0) {
                const uint8_t blank[16] = {0};
                result = installer_guided_plan(disk->diskseq, disk->bytes,
                    disk->logical_block,
                    disk->gpt ? disk->gpt->disk_guid : blank, &plan);
            } else {
                if (!disk->gpt) { result = -EINVAL; break; }
                clear_screen(&s);
                box = centered_box(&s, 18, 76);
                draw_box(&s, box, "Manual Partition Selection");
                size_t esp = 0, root = disk->gpt->partition_count > 1 ? 1 : 0;
                result = select_partition(read_key, input_context, &s, box,
                    disk->gpt, "Select the existing EFI System Partition",
                    &esp);
                if (!result)
                    result = select_partition(read_key, input_context, &s, box,
                        disk->gpt, "Select the partition to format as ext4",
                        &root);
                if (result == -ECANCELED) {
                    result = 0; page = PAGE_LAYOUT; continue;
                }
                if (result) break;
                clear_screen(&s);
                draw_box(&s, box, "Manual Partition Selection");
                box_text(&s, box, 4,
                    "The selected root partition will be formatted as ext4.");
                box_text(&s, box, 7, "Format the selected ESP as FAT32?");
                unsigned format_choice = 1;
                result = choose_two(read_key, input_context, &s, box, 9,
                    "Yes", "No", &format_choice, 'y', 'n');
                if (result == -ECANCELED) {
                    result = 0; page = PAGE_LAYOUT; continue;
                }
                if (result) break;
                const int format_esp = format_choice == 0;
                result = installer_manual_plan(disk->diskseq, disk->bytes,
                    disk->logical_block, disk->gpt, esp, root,
                    format_esp, 1, &plan);
                if (!result && !format_esp)
                    result = disk->read_at ? installer_probe_fat32(
                        disk->read_at, disk->read_context, disk->bytes,
                        disk->logical_block, plan.esp.first_lba,
                        plan.esp.last_lba) : -EOPNOTSUPP;
            }
            if (result) break;
            page = PAGE_CONFIRM;
            continue;
        }

        clear_screen(&s);
        struct ui_box box = centered_box(&s, 18, 78);
        draw_box(&s, box, "Confirm Installation");
        char summary[160], extent[192];
        disk_summary(summary, sizeof(summary), target, disk);
        box_text(&s, box, 2, summary);
        unsigned row = 5;
        if (plan.kind == INSTALLER_LAYOUT_GUIDED) {
            extent_summary(extent, sizeof(extent), "ERASE ENTIRE DISK",
                plan.erase, plan.logical_block);
            box_line(&s, box, row++, "%s", extent);
            extent_summary(extent, sizeof(extent), "New ESP (FAT32)",
                plan.esp, plan.logical_block);
            box_line(&s, box, row++, "%s", extent);
            extent_summary(extent, sizeof(extent), "New root (ext4)",
                plan.root, plan.logical_block);
            box_line(&s, box, row++, "%s", extent);
        } else {
            extent_summary(extent, sizeof(extent),
                plan.format_esp ? "FORMAT ESP" : "Reuse ESP",
                plan.esp, plan.logical_block);
            box_line(&s, box, row++, "%s", extent);
            extent_summary(extent, sizeof(extent), "FORMAT root (ext4)",
                plan.root, plan.logical_block);
            box_line(&s, box, row++, "%s", extent);
        }
        char expected[32], entered[32];
        /* The short phrase proves deliberate destructive intent.  Device
         * identity is not weakened: execute() independently rechecks the
         * selected resource, generation, namespace, capacity and GPT before
         * it grants any write authority. */
        snprintf(expected, sizeof(expected), "ERASE %zu", target);
        fputs("\033[1;31m", output);
        box_text(&s, box, 10,
            "WARNING: the listed data will be destroyed.");
        fputs("\033[0m", output);
        box_line(&s, box, 12, "Type %s to confirm this disk.", expected);
        box_text(&s, box, 16, "Esc: back");
        result = edit_line(read_key, input_context, &s, box, 14,
            "Confirmation: ", entered, sizeof(entered));
        if (result == -ECANCELED) {
            result = 0; page = PAGE_LAYOUT; continue;
        }
        if (result) break;
        if (strcmp(entered, expected)) {
            /* A typo is not an installer failure and must never advance to
             * the write path.  Keep the target/layout selection and ask
             * again; the serial diagnostic makes keyboard-layout or TTY
             * translation problems observable without weakening the exact
             * destructive-action confirmation. */
            fprintf(stderr,
                "[installer] confirmation mismatch expected=\"%s\" entered=\"%s\"\n",
                expected, entered);
            result = 0;
            continue;
        }

        clear_screen(&s);
        box = centered_box(&s, 12, 70);
        draw_box(&s, box, "Installing PachaOS");
        box_text(&s, box, 8,
            "Do not remove the installation media.");
        struct ui_progress progress = {.screen = &s, .box = box};
        snprintf(progress.phase, sizeof(progress.phase),
            "Confirmation accepted; preparing installation");
        draw_install_progress(&progress, 0, NULL);
        /* The executor owns a fresh identity/GPT read and must fail closed if
         * media changed between confirmation and its first write. */
        result = execute(context, target, &plan,
            draw_install_progress, &progress);
        result = result ? result : INSTALLER_UI_DONE;
        break;
    }
    screen_end(&s);
    return result;
}
