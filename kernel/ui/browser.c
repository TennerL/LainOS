#include <stdint.h>
#include "browser.h"
#include "kernel.h"
#include "keyboard.h"
#include "lainfs.h"
#include "mouse.h"

#define BROWSER_MAX_ENTRIES 512u
#define BROWSER_NAME_SIZE 32u
#define BROWSER_PATH_SIZE 128u
#define BROWSER_RENDER_COLS 256u
#define BROWSER_RENDER_ROWS 128u
#define BROWSER_COPY_BUFFER_SIZE 262144u

typedef struct {
    char drive;
    uint32_t dir_id;
    uint32_t selected;
    uint32_t top;
    char path[BROWSER_PATH_SIZE];
} browser_pane_t;

typedef struct {
    char name[BROWSER_NAME_SIZE];
    uint32_t type;
    uint32_t size;
} browser_entry_t;

static browser_pane_t panes[2];
static browser_entry_t entries[2][BROWSER_MAX_ENTRIES];
static uint32_t entry_counts[2];
static unsigned int active_pane;
static const char *status_message;
static char browser_copy_buffer[BROWSER_COPY_BUFFER_SIZE];
static char rendered_cells[BROWSER_RENDER_ROWS][BROWSER_RENDER_COLS];
static uint8_t rendered_cell_valid[BROWSER_RENDER_ROWS][BROWSER_RENDER_COLS];
static unsigned int rendered_cols;
static unsigned int rendered_rows;

static void enter_selected(void);
static void go_up(void);

static uint32_t str_len(const char *s) {
    uint32_t len = 0;
    while (s[len]) {
        ++len;
    }
    return len;
}

static void copy_text(char *dst, uint32_t dst_size, const char *src) {
    uint32_t i = 0;

    if (dst_size == 0) {
        return;
    }

    if (src == 0 || *src == '\0') {
        dst[0] = '\\';
        if (dst_size > 1u) {
            dst[1] = '\0';
        }
        return;
    }

    while (src[i] && i + 1u < dst_size) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void reset_path(browser_pane_t *pane) {
    pane->path[0] = '\\';
    pane->path[1] = '\0';
}

static void append_path(browser_pane_t *pane, const char *name) {
    uint32_t len = str_len(pane->path);
    uint32_t i = 0;

    if (len > 1u && len + 1u < BROWSER_PATH_SIZE) {
        pane->path[len++] = '\\';
        pane->path[len] = '\0';
    }

    while (name[i] && len + 1u < BROWSER_PATH_SIZE) {
        pane->path[len++] = name[i++];
    }
    pane->path[len] = '\0';
}

static void pop_path(browser_pane_t *pane) {
    uint32_t len = str_len(pane->path);

    if (len <= 1u) {
        reset_path(pane);
        return;
    }

    while (len > 1u && pane->path[len - 1u] != '\\') {
        --len;
    }

    if (len <= 1u) {
        pane->path[1] = '\0';
    } else {
        pane->path[len - 1u] = '\0';
    }
}

static void fill_line(char *line, unsigned int cols) {
    for (unsigned int i = 0; i < cols; ++i) {
        line[i] = ' ';
    }
}

static void put_text(char *line, unsigned int cols, unsigned int *col, const char *text) {
    while (*text && *col < cols) {
        line[*col] = *text;
        ++(*col);
        ++text;
    }
}

static void put_dec(char *line, unsigned int cols, unsigned int *col, uint32_t value) {
    char digits[10];
    uint32_t count = 0;

    if (value == 0) {
        if (*col < cols) {
            line[(*col)++] = '0';
        }
        return;
    }

    while (value != 0 && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (count > 0 && *col < cols) {
        line[(*col)++] = digits[--count];
    }
}

static void draw_line_at(unsigned int x, unsigned int row, const char *line, unsigned int cols) {
    if (row >= BROWSER_RENDER_ROWS) {
        return;
    }

    for (unsigned int col = 0; col < cols; ++col) {
        unsigned int screen_col = x + col;

        if (screen_col >= BROWSER_RENDER_COLS) {
            break;
        }

        if (!rendered_cell_valid[row][screen_col] ||
            rendered_cells[row][screen_col] != line[col]) {
            console_put_char_at(screen_col, row, line[col]);
            rendered_cells[row][screen_col] = line[col];
            rendered_cell_valid[row][screen_col] = 1;
        }
    }
}

static void draw_char_at(unsigned int col, unsigned int row, char ch) {
    if (row >= BROWSER_RENDER_ROWS || col >= BROWSER_RENDER_COLS) {
        return;
    }

    if (!rendered_cell_valid[row][col] || rendered_cells[row][col] != ch) {
        console_put_char_at(col, row, ch);
        rendered_cells[row][col] = ch;
        rendered_cell_valid[row][col] = 1;
    }
}

static void invalidate_render_cache(void) {
    for (unsigned int row = 0; row < BROWSER_RENDER_ROWS; ++row) {
        for (unsigned int col = 0; col < BROWSER_RENDER_COLS; ++col) {
            rendered_cell_valid[row][col] = 0;
        }
    }
}

static void note_browser_geometry(unsigned int cols, unsigned int rows) {
    if (cols != rendered_cols || rows != rendered_rows) {
        rendered_cols = cols;
        rendered_rows = rows;
        invalidate_render_cache();
    }
}

static void load_entries(unsigned int pane_index) {
    uint32_t count = 0;

    entry_counts[pane_index] = 0;
    if (lainfs_child_count(panes[pane_index].drive, panes[pane_index].dir_id, &count) != 0) {
        status_message = "could not read directory";
        return;
    }

    if (count > BROWSER_MAX_ENTRIES) {
        count = BROWSER_MAX_ENTRIES;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (lainfs_child_info(panes[pane_index].drive,
                              panes[pane_index].dir_id,
                              i,
                              entries[pane_index][i].name,
                              sizeof(entries[pane_index][i].name),
                              &entries[pane_index][i].type,
                              &entries[pane_index][i].size) != 0) {
            count = i;
            status_message = "could not read directory entry";
            break;
        }
    }

    entry_counts[pane_index] = count;
    if (panes[pane_index].selected >= count && count > 0u) {
        panes[pane_index].selected = count - 1u;
    }
    if (count == 0u) {
        panes[pane_index].selected = 0;
        panes[pane_index].top = 0;
    }
}

static void reload_all(void) {
    load_entries(0);
    load_entries(1);
}

static void ensure_visible(unsigned int pane_index, uint32_t visible_rows) {
    browser_pane_t *pane = &panes[pane_index];

    if (visible_rows == 0u) {
        pane->top = 0;
        return;
    }

    if (pane->selected < pane->top) {
        pane->top = pane->selected;
    } else if (pane->selected >= pane->top + visible_rows) {
        pane->top = pane->selected - visible_rows + 1u;
    }
}

static void draw_pane(unsigned int pane_index,
                      unsigned int x,
                      unsigned int width,
                      unsigned int body_top,
                      unsigned int body_rows) {
    char line[BROWSER_RENDER_COLS];
    unsigned int col = 0;
    browser_pane_t *pane = &panes[pane_index];

    if (width > BROWSER_RENDER_COLS) {
        width = BROWSER_RENDER_COLS;
    }

    fill_line(line, width);
    if (pane_index == active_pane) {
        put_text(line, width, &col, ">");
    } else {
        put_text(line, width, &col, " ");
    }
    put_text(line, width, &col, pane_index == 0u ? " LEFT  " : " RIGHT ");
    line[col < width ? col : width - 1u] = pane->drive;
    if (col + 1u < width) {
        line[col + 1u] = ':';
    }
    col += 2u;
    if (col < width) {
        line[col++] = ' ';
    }
    put_text(line, width, &col, pane->path);
    draw_line_at(x, 0, line, width);

    fill_line(line, width);
    col = 0;
    put_text(line, width, &col, "TYPE  NAME                       SIZE");
    draw_line_at(x, 1, line, width);

    ensure_visible(pane_index, body_rows);
    for (unsigned int row = 0; row < body_rows; ++row) {
        uint32_t index = pane->top + row;

        fill_line(line, width);
        col = 0;

        if (index < entry_counts[pane_index]) {
            browser_entry_t *entry = &entries[pane_index][index];
            if (index == pane->selected) {
                put_text(line, width, &col, pane_index == active_pane ? "> " : "- ");
            } else {
                put_text(line, width, &col, "  ");
            }

            put_text(line, width, &col, entry->type == LAINFS_ENTRY_TYPE_DIR ? "DIR   " : "FILE  ");
            put_text(line, width, &col, entry->name);
            while (col < 32u && col < width) {
                line[col++] = ' ';
            }
            if (entry->type == LAINFS_ENTRY_TYPE_DIR) {
                put_text(line, width, &col, "-");
            } else {
                put_dec(line, width, &col, entry->size);
            }
        } else if (entry_counts[pane_index] == 0u && row == 0u) {
            put_text(line, width, &col, "  empty");
        }

        for (unsigned int i = 0; i < width; ++i) {
            console_put_char_at(x + i, body_top + row, line[i]);
        }
    }
}

static void draw_browser(void) {
    unsigned int rows = console_rows();
    unsigned int cols = console_columns();
    unsigned int left_width;
    unsigned int right_x;
    unsigned int right_width;
    unsigned int body_rows;
    char line[BROWSER_RENDER_COLS];
    unsigned int col = 0;

    if (cols > BROWSER_RENDER_COLS) {
        cols = BROWSER_RENDER_COLS;
    }
    if (rows > BROWSER_RENDER_ROWS) {
        rows = BROWSER_RENDER_ROWS;
    }
    if (rows < 4u || cols < 20u) {
        return;
    }

    note_browser_geometry(cols, rows);
    left_width = cols / 2u;
    right_x = left_width + 1u;
    right_width = cols - right_x;
    body_rows = rows - 3u;

    console_cursor_enable(0);
    draw_pane(0, 0, left_width, 2, body_rows);
    draw_pane(1, right_x, right_width, 2, body_rows);

    for (unsigned int row = 0; row + 1u < rows; ++row) {
        draw_char_at(left_width, row, '|');
    }

    fill_line(line, cols);
    put_text(line, cols, &col, status_message ? status_message : "Tab pane  Enter open  Backspace up  C copy  M move  Esc exit");
    draw_line_at(0, rows - 1u, line, cols);
}

static int browser_click(uint32_t mouse_x, uint32_t mouse_y, int buttons) {
    unsigned int rows = console_rows();
    unsigned int cols = console_columns();
    unsigned int col = 0;
    unsigned int row = 0;
    unsigned int left_width;
    unsigned int right_x;
    unsigned int pane_index;
    unsigned int pane_col;
    unsigned int body_rows;
    uint32_t index;

    if (!console_point_to_cell(mouse_x, mouse_y, &col, &row)) {
        return 0;
    }
    if (cols > BROWSER_RENDER_COLS) {
        cols = BROWSER_RENDER_COLS;
    }
    if (rows > BROWSER_RENDER_ROWS) {
        rows = BROWSER_RENDER_ROWS;
    }
    if (rows < 4u || cols < 20u) {
        return 0;
    }

    left_width = cols / 2u;
    right_x = left_width + 1u;
    body_rows = rows - 3u;

    if (row + 1u == rows) {
        return 0;
    }
    if (col < left_width) {
        pane_index = 0;
        pane_col = col;
    } else if (col >= right_x) {
        pane_index = 1;
        pane_col = col - right_x;
    } else {
        return 0;
    }
    (void)pane_col;

    if ((buttons & MOUSE_RIGHT) != 0) {
        active_pane = pane_index;
        go_up();
        return 1;
    }

    if ((buttons & MOUSE_LEFT) == 0) {
        return 0;
    }

    if (row < 2u) {
        status_message = 0;
        active_pane = pane_index;
        return 1;
    }

    row -= 2u;
    if (row >= body_rows) {
        return 0;
    }

    active_pane = pane_index;
    index = panes[pane_index].top + row;
    if (index >= entry_counts[pane_index]) {
        return 1;
    }

    if (panes[pane_index].selected == index) {
        enter_selected();
    } else {
        status_message = 0;
        panes[pane_index].selected = index;
    }

    return 1;
}

static int move_selection(int delta) {
    browser_pane_t *pane = &panes[active_pane];
    uint32_t count = entry_counts[active_pane];
    int had_status = status_message != 0;

    status_message = 0;
    if (count == 0u) {
        return had_status;
    }

    if (delta < 0) {
        if (pane->selected > 0u) {
            --pane->selected;
            return 1;
        }
    } else if (pane->selected + 1u < count) {
        ++pane->selected;
        return 1;
    }

    return had_status;
}

static void enter_selected(void) {
    browser_pane_t *pane = &panes[active_pane];
    browser_entry_t *entry;
    uint32_t next_dir = LAINFS_ROOT_DIR;

    status_message = 0;
    if (entry_counts[active_pane] == 0u) {
        return;
    }

    entry = &entries[active_pane][pane->selected];
    if (entry->type != LAINFS_ENTRY_TYPE_DIR) {
        status_message = "selected item is not a directory";
        return;
    }

    if (lainfs_find_dir(pane->drive, pane->dir_id, entry->name, &next_dir) != 0) {
        status_message = "could not open directory";
        return;
    }

    pane->dir_id = next_dir;
    pane->selected = 0;
    pane->top = 0;
    append_path(pane, entry->name);
    load_entries(active_pane);
}

static void go_up(void) {
    browser_pane_t *pane = &panes[active_pane];
    uint32_t parent = LAINFS_ROOT_DIR;

    status_message = 0;
    if (pane->dir_id == LAINFS_ROOT_DIR) {
        return;
    }

    if (lainfs_parent_dir(pane->drive, pane->dir_id, &parent) != 0) {
        status_message = "could not move to parent directory";
        return;
    }

    pane->dir_id = parent;
    pane->selected = 0;
    pane->top = 0;
    pop_path(pane);
    load_entries(active_pane);
}

static void move_to_other_pane(void) {
    browser_pane_t *source = &panes[active_pane];
    browser_pane_t *target = &panes[active_pane == 0u ? 1u : 0u];
    browser_entry_t *entry;
    int status;

    status_message = 0;
    if (entry_counts[active_pane] == 0u) {
        return;
    }

    if (source->drive == target->drive && source->dir_id == target->dir_id) {
        status_message = "source and destination are the same";
        return;
    }

    entry = &entries[active_pane][source->selected];
    if (source->drive != target->drive) {
        uint32_t size = 0;

        if (entry->type != LAINFS_ENTRY_TYPE_FILE) {
            status_message = "cross-drive move only supports files";
            return;
        }

        if (lainfs_load_file_in_dir(source->drive,
                                    source->dir_id,
                                    entry->name,
                                    browser_copy_buffer,
                                    sizeof(browser_copy_buffer),
                                    &size) != 0 ||
            lainfs_save_file_in_dir(target->drive,
                                    target->dir_id,
                                    entry->name,
                                    browser_copy_buffer,
                                    size) != 0 ||
            lainfs_delete_in_dir(source->drive, source->dir_id, entry->name) != 0) {
            status_message = "cross-drive move failed";
            return;
        }

        status_message = "moved across drives";
        reload_all();
        return;
    }

    status = lainfs_rename_in_dir(source->drive,
                                  source->dir_id,
                                  entry->name,
                                  target->dir_id,
                                  entry->name);
    if (status == 0) {
        status_message = "moved";
        reload_all();
    } else if (status == -6) {
        status_message = "move failed: target exists";
    } else if (status == -8) {
        status_message = "move failed: cannot move a directory into itself";
    } else {
        status_message = "move failed";
    }
}

static void copy_to_other_pane(void) {
    browser_pane_t *source = &panes[active_pane];
    browser_pane_t *target = &panes[active_pane == 0u ? 1u : 0u];
    browser_entry_t *entry;
    uint32_t size = 0;

    status_message = 0;
    if (entry_counts[active_pane] == 0u) {
        return;
    }

    entry = &entries[active_pane][source->selected];
    if (entry->type != LAINFS_ENTRY_TYPE_FILE) {
        status_message = "copy only supports files";
        return;
    }

    if (source->drive == target->drive && source->dir_id == target->dir_id) {
        status_message = "source and destination are the same";
        return;
    }

    if (lainfs_load_file_in_dir(source->drive,
                                source->dir_id,
                                entry->name,
                                browser_copy_buffer,
                                sizeof(browser_copy_buffer),
                                &size) != 0) {
        status_message = "copy failed: could not read source";
        return;
    }

    if (lainfs_save_file_in_dir(target->drive,
                                target->dir_id,
                                entry->name,
                                browser_copy_buffer,
                                size) != 0) {
        status_message = "copy failed: could not write target";
        return;
    }

    status_message = "copied";
    reload_all();
}

int browser_run(char left_drive,
                uint32_t left_dir,
                const char *left_path,
                char right_drive,
                uint32_t right_dir,
                const char *right_path) {
    active_pane = 0;
    status_message = 0;

    panes[0].drive = left_drive;
    panes[0].dir_id = left_dir;
    panes[0].selected = 0;
    panes[0].top = 0;
    copy_text(panes[0].path, sizeof(panes[0].path), left_path);

    panes[1].drive = right_drive;
    panes[1].dir_id = right_dir;
    panes[1].selected = 0;
    panes[1].top = 0;
    copy_text(panes[1].path, sizeof(panes[1].path), right_path);

    reload_all();
    console_clear();
    invalidate_render_cache();
    draw_browser();

    int last_buttons = mouse_buttons();
    for (;;) {
        key_event_t key;
        int changed = 0;

        while (keyboard_poll_key(&key)) {
            if (key.type == KEY_ESC || key.type == KEY_CTRL_Q) {
                console_cursor_enable(0);
                console_clear();
                return 0;
            }

            if (key.type == KEY_TAB) {
                status_message = 0;
                active_pane = active_pane == 0u ? 1u : 0u;
                changed = 1;
            } else if (key.type == KEY_UP) {
                changed = move_selection(-1) || changed;
            } else if (key.type == KEY_DOWN) {
                changed = move_selection(1) || changed;
            } else if (key.type == KEY_ENTER || (key.type == KEY_CHAR && key.ch == 'o')) {
                enter_selected();
                changed = 1;
            } else if (key.type == KEY_BACKSPACE || (key.type == KEY_CHAR && key.ch == 'u')) {
                go_up();
                changed = 1;
            } else if (key.type == KEY_CHAR && (key.ch == 'm' || key.ch == 'M')) {
                move_to_other_pane();
                changed = 1;
            } else if (key.type == KEY_CHAR && (key.ch == 'c' || key.ch == 'C')) {
                copy_to_other_pane();
                changed = 1;
            }
        }

        int buttons = mouse_buttons();
        int pressed = buttons & ~last_buttons;
        if ((pressed & (MOUSE_LEFT | MOUSE_RIGHT)) != 0) {
            changed = browser_click((uint32_t)mouse_x(), (uint32_t)mouse_y(), buttons) || changed;
        }
        last_buttons = buttons;

        if (changed) {
            draw_browser();
        }

        __asm__ __volatile__("pause");
    }
}
