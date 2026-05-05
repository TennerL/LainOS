#include <stdint.h>
#include "editor.h"
#include "kernel.h"
#include "keyboard.h"
#include "lainfs.h"

#define EDITOR_BUFFER_SIZE 262144u
#define EDITOR_RENDER_MAX_ROWS 128u
#define EDITOR_RENDER_MAX_COLS 256u

static char buffer[EDITOR_BUFFER_SIZE];
static uint32_t buffer_size;
static uint32_t cursor_index;
static uint32_t top_line;
static int modified;
static char current_drive;
static uint32_t current_parent_id;
static const char *current_name;
static const char *status_message;
static char rendered_text[EDITOR_RENDER_MAX_ROWS][EDITOR_RENDER_MAX_COLS];
static char rendered_status[EDITOR_RENDER_MAX_COLS];
static unsigned int rendered_cols;
static unsigned int rendered_text_rows;
static int text_row_valid[EDITOR_RENDER_MAX_ROWS];
static int status_valid;
static unsigned int rendered_cursor_row;
static int rendered_cursor_valid;

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static void index_to_line_col(uint32_t index, uint32_t *out_line, uint32_t *out_col) {
    uint32_t line = 0;
    uint32_t col = 0;

    if (index > buffer_size) {
        index = buffer_size;
    }

    for (uint32_t i = 0; i < index; ++i) {
        if (buffer[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }

    *out_line = line;
    *out_col = col;
}

static uint32_t line_start_index(uint32_t target_line) {
    uint32_t line = 0;

    if (target_line == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < buffer_size; ++i) {
        if (buffer[i] == '\n') {
            ++line;
            if (line == target_line) {
                return i + 1;
            }
        }
    }

    return buffer_size;
}

static uint32_t line_length(uint32_t target_line) {
    uint32_t start = line_start_index(target_line);
    uint32_t len = 0;

    while (start + len < buffer_size && buffer[start + len] != '\n') {
        ++len;
    }

    return len;
}

static int line_exists(uint32_t target_line) {
    uint32_t line = 0;

    if (target_line == 0) {
        return 1;
    }

    for (uint32_t i = 0; i < buffer_size; ++i) {
        if (buffer[i] == '\n') {
            ++line;
            if (line == target_line) {
                return 1;
            }
        }
    }

    return 0;
}

static uint32_t line_col_to_index(uint32_t target_line, uint32_t target_col) {
    uint32_t start = line_start_index(target_line);
    uint32_t len = line_length(target_line);

    return start + min_u32(target_col, len);
}

static void insert_char(char ch) {
    if (buffer_size >= EDITOR_BUFFER_SIZE) {
        return;
    }

    for (uint32_t i = buffer_size; i > cursor_index; --i) {
        buffer[i] = buffer[i - 1];
    }

    buffer[cursor_index] = ch;
    ++cursor_index;
    ++buffer_size;
    modified = 1;
}

static void delete_before_cursor(void) {
    if (cursor_index == 0) {
        return;
    }

    for (uint32_t i = cursor_index - 1; i + 1 < buffer_size; ++i) {
        buffer[i] = buffer[i + 1];
    }

    --cursor_index;
    --buffer_size;
    modified = 1;
}

static void move_up(void) {
    uint32_t line = 0;
    uint32_t col = 0;

    index_to_line_col(cursor_index, &line, &col);
    if (line == 0) {
        return;
    }

    cursor_index = line_col_to_index(line - 1, col);
}

static void move_down(void) {
    uint32_t line = 0;
    uint32_t col = 0;

    index_to_line_col(cursor_index, &line, &col);
    if (!line_exists(line + 1)) {
        return;
    }

    cursor_index = line_col_to_index(line + 1, col);
}

static void ensure_cursor_visible(void) {
    uint32_t line = 0;
    uint32_t col = 0;
    unsigned int rows = console_rows();
    uint32_t text_rows = rows > 1 ? rows - 1 : rows;

    (void)col;
    index_to_line_col(cursor_index, &line, &col);

    if (text_rows == 0) {
        top_line = 0;
        return;
    }

    if (line < top_line) {
        top_line = line;
    } else if (line >= top_line + text_rows) {
        top_line = line - text_rows + 1;
    }
}

static void invalidate_render_cache(void) {
    for (unsigned int row = 0; row < EDITOR_RENDER_MAX_ROWS; ++row) {
        text_row_valid[row] = 0;
    }
    status_valid = 0;
    rendered_cursor_valid = 0;
}

static void render_put_char(char *line, unsigned int cols, unsigned int *col, char ch) {
    if (*col < cols) {
        line[*col] = ch;
        ++(*col);
    }
}

static void render_put_string(char *line, unsigned int cols, unsigned int *col, const char *s) {
    while (*s && *col < cols) {
        line[*col] = *s;
        ++(*col);
        ++s;
    }
}

static void render_put_hex64(char *line, unsigned int cols, unsigned int *col, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";

    for (unsigned int i = 0; i < 16 && *col < cols; ++i) {
        unsigned int shift = (15u - i) * 4u;
        line[*col] = digits[(value >> shift) & 0xFu];
        ++(*col);
    }
}

static void draw_cached_row(unsigned int row,
                            char *cached,
                            int *valid,
                            const char *next,
                            unsigned int cols) {
    for (unsigned int col = 0; col < cols; ++col) {
        if (!*valid || cached[col] != next[col]) {
            console_put_char_at(col, row, next[col]);
            cached[col] = next[col];
        }
    }

    *valid = 1;
}

static void draw_status(void) {
    unsigned int rows = console_rows();
    unsigned int cols = console_columns();
    unsigned int status_row = rows > 0 ? rows - 1 : 0;
    unsigned int col = 0;
    char next[EDITOR_RENDER_MAX_COLS];

    if (rows == 0) {
        return;
    }

    if (cols > EDITOR_RENDER_MAX_COLS) {
        cols = EDITOR_RENDER_MAX_COLS;
    }

    for (unsigned int i = 0; i < cols; ++i) {
        next[i] = ' ';
    }

    if (status_message) {
        render_put_string(next, cols, &col, status_message);
    } else {
        render_put_char(next, cols, &col, current_drive);
        render_put_char(next, cols, &col, ':');
        render_put_char(next, cols, &col, '\\');

        render_put_string(next, cols, &col, current_name);

        if (modified) {
            render_put_char(next, cols, &col, '*');
        }
    }

    if (cols > 55) {
        col = cols - 55;
        render_put_string(next, cols, &col, "buf=0x");
        render_put_hex64(next, cols, &col, (uint64_t)(uintptr_t)buffer);
    }

    if (cols > 25) {
        col = cols - 25;
        render_put_string(next, cols, &col, "Ctrl+S Save  Esc Exit");
    }

    draw_cached_row(status_row, rendered_status, &status_valid, next, cols);
}

static void draw_text(void) {
    unsigned int cols = console_columns();
    unsigned int rows = console_rows();
    unsigned int text_rows = rows > 1 ? rows - 1 : rows;

    if (cols > EDITOR_RENDER_MAX_COLS) {
        cols = EDITOR_RENDER_MAX_COLS;
    }

    if (text_rows > EDITOR_RENDER_MAX_ROWS) {
        text_rows = EDITOR_RENDER_MAX_ROWS;
    }

    for (unsigned int row = 0; row < text_rows; ++row) {
        unsigned int col = 0;
        char next[EDITOR_RENDER_MAX_COLS];
        uint32_t index = line_start_index(top_line + row);

        for (unsigned int i = 0; i < cols; ++i) {
            next[i] = ' ';
        }

        while (index < buffer_size && buffer[index] != '\n' && col < cols) {
            next[col++] = buffer[index++];
        }

        draw_cached_row(row, rendered_text[row], &text_row_valid[row], next, cols);
    }

    for (unsigned int row = text_rows; row < rendered_text_rows && row < EDITOR_RENDER_MAX_ROWS; ++row) {
        char next[EDITOR_RENDER_MAX_COLS];

        for (unsigned int col = 0; col < cols; ++col) {
            next[col] = ' ';
        }

        draw_cached_row(row, rendered_text[row], &text_row_valid[row], next, cols);
    }
}

static void note_editor_geometry(void) {
    unsigned int cols = console_columns();
    unsigned int rows = console_rows();
    unsigned int text_rows = rows > 1 ? rows - 1 : rows;

    if (cols > EDITOR_RENDER_MAX_COLS) {
        cols = EDITOR_RENDER_MAX_COLS;
    }

    if (text_rows > EDITOR_RENDER_MAX_ROWS) {
        text_rows = EDITOR_RENDER_MAX_ROWS;
    }

    if (cols != rendered_cols || text_rows != rendered_text_rows) {
        rendered_cols = cols;
        rendered_text_rows = text_rows;
        invalidate_render_cache();
    }
}

static void draw_editor(void) {
    uint32_t cursor_line = 0;
    uint32_t cursor_col = 0;
    unsigned int rows = console_rows();
    unsigned int text_rows = rows > 1 ? rows - 1 : rows;

    ensure_cursor_visible();
    note_editor_geometry();

    if (rendered_cursor_valid && rendered_cursor_row < EDITOR_RENDER_MAX_ROWS) {
        text_row_valid[rendered_cursor_row] = 0;
    }

    console_cursor_enable(0);
    draw_text();
    draw_status();

    index_to_line_col(cursor_index, &cursor_line, &cursor_col);
    if (cursor_line >= top_line && cursor_line < top_line + text_rows) {
        unsigned int screen_row = cursor_line - top_line;
        console_set_cursor(cursor_col, screen_row);
        rendered_cursor_row = screen_row;
        rendered_cursor_valid = 1;
    } else {
        rendered_cursor_valid = 0;
    }
    console_cursor_enable(1);
}

static void set_save_status(int status) {
    if (status == 0) {
        status_message = "saved";
    } else if (status == -2) {
        status_message = "save failed: file too large";
    } else if (status == -3) {
        status_message = "save failed: drive is not lainfs";
    } else if (status == -5) {
        status_message = "save failed: directory is full";
    } else if (status == -9) {
        status_message = "save failed: disk is full";
    } else {
        status_message = "save failed";
    }
}

int editor_run(char drive_letter, const char *name) {
    return editor_run_in_dir(drive_letter, LAINFS_ROOT_DIR, name);
}

int editor_run_in_dir(char drive_letter, uint32_t parent_id, const char *name) {
    int status;

    if (!name || name[0] == '\0') {
        return -1;
    }

    current_drive = drive_letter;
    current_parent_id = parent_id;
    current_name = name;
    buffer_size = 0;
    cursor_index = 0;
    top_line = 0;
    modified = 0;
    status_message = 0;
    rendered_cols = 0;
    rendered_text_rows = 0;
    rendered_cursor_valid = 0;
    invalidate_render_cache();

    status = lainfs_load_file_in_dir(drive_letter, parent_id, name, buffer, EDITOR_BUFFER_SIZE, &buffer_size);
    if (status == -5) {
        buffer_size = 0;
    } else if (status != 0) {
        return status;
    }

    if (cursor_index > buffer_size) {
        cursor_index = buffer_size;
    }

    console_clear();


    for (;;) {
        key_event_t key;

        draw_editor();

        key = keyboard_read_key();

        if (key.type == KEY_CTRL_S) {
            status = lainfs_save_file_in_dir(current_drive, current_parent_id, current_name, buffer, buffer_size);
            if (status == 0) {
                modified = 0;
            }
            set_save_status(status);
            continue;
        }

        if (key.type == KEY_ESC || key.type == KEY_CTRL_Q) {
            console_cursor_enable(0);
            console_clear();
            return 0;
        }

        if (key.type == KEY_LEFT) {
            status_message = 0;
            if (cursor_index > 0) {
                --cursor_index;
            }
        } else if (key.type == KEY_RIGHT) {
            status_message = 0;
            if (cursor_index < buffer_size) {
                ++cursor_index;
            }
        } else if (key.type == KEY_UP) {
            status_message = 0;
            move_up();
        } else if (key.type == KEY_DOWN) {
            status_message = 0;
            move_down();
        } else if (key.type == KEY_BACKSPACE) {
            status_message = 0;
            delete_before_cursor();
        } else if (key.type == KEY_ENTER) {
            status_message = 0;
            insert_char('\n');
        } else if (key.type == KEY_TAB) {
            status_message = 0;
            insert_char(' ');
            insert_char(' ');
            insert_char(' ');
            insert_char(' ');
        } else if (key.type == KEY_CHAR) {
            status_message = 0;
            insert_char(key.ch);
        }
    }
}
