/**
 * ABCZed - A lightweight terminal-based text editor using C.
 * Copyright (c) 2025 Cyril John Magayaga
 * 
 * Custom keybindings:
 *   cc - Enter insert mode
 *   Ctrl+K - Copy
 *   Ctrl+V - Paste
 *   Ctrl+Z - Undo
 *   Ctrl+Y - Redo
 *   Ctrl+A - Select all
 *   Ctrl+H - Show help
 *   Ctrl+Shift+H - Show about information
 *   Ctrl+Shift+Q - Quit
 *   Ctrl+Shift++ - Text larger
 *   Ctrl+Shift+- - Text smaller
 *
 * Compile with: gcc -o abczed abczed.c -lncurses
 * Usage: ./abc_vi [filename]
 */

/* Enable POSIX.1-2008 features on Linux */
#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

/* Safe implementation of strdup if not available */
#ifndef HAVE_STRDUP
#include <string.h>
#include <stdlib.h>

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *new = malloc(len);
    if (new == NULL) return NULL;
    return (char *)memcpy(new, s, len);
}
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <signal.h>  // For signal handling
#include <termios.h> // For terminal I/O
#include <sys/ioctl.h>
#include <unistd.h>
#include <ncurses.h>

/* Define key codes */
#define CTRL_KEY(k) ((k) & 0x1f)

/* Editor modes */
enum editor_mode {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_COMMAND,
    MODE_SELECTION  // New mode for text selection
};

/* Undo/Redo operation types */
enum operation_type {
    OP_INSERT_CHAR,
    OP_DELETE_CHAR,
    OP_INSERT_LINE,
    OP_DELETE_LINE,
    OP_NEWLINE
};

/* Undo/Redo operation structure */
typedef struct operation {
    enum operation_type type;
    int cx, cy;           // Cursor position
    char c;               // Character (for insert/delete char)
    char *line;           // Line content (for insert/delete line)
    int line_size;        // Line size
    struct operation *next;
} operation;

/* Data structure for a single line of text */
typedef struct erow {
    int size;
    char *chars;
} erow;

/* Editor configuration structure */
typedef struct editor_config {
    int cx, cy;                  /* Cursor x and y position */
    int rowoff;                  /* Row offset */
    int coloff;                  /* Column offset */
    int screenrows;              /* Number of rows that we can show */
    int screencols;             /* Number of columns that we can show */
    int numrows;                /* Number of rows */
    erow *row;                  /* Rows */
    int dirty;                  /* File modified but not saved */
    char *filename;             /* Currently open filename */
    char statusmsg[80];         /* Status message */
    time_t statusmsg_time;      /* When to clear status message */
    enum editor_mode mode;       /* Current editor mode */
    char **clipboard;           /* Array of lines in clipboard */
    int clipboard_len;          /* Number of lines in clipboard */
    int show_line_numbers;      /* Whether to show line numbers */
    int font_size;              /* Font size for display */
    char commandbuf[256];       /* Buffer for command input */
    int commandlen;             /* Length of command in buffer */
    int sel_start_x, sel_start_y; /* Selection start position */
    int sel_end_x, sel_end_y;   /* Selection end position */
    int selecting;              /* Currently selecting text */
    operation *undo_stack;      /* Stack for undo operations */
    operation *redo_stack;      /* Stack for redo operations */
    struct termios orig_termios; /* Original terminal settings */
} editor_config;

editor_config E;

/* Function prototype for cleanup to avoid implicit declaration warning */
void editor_cleanup();

/* Error handling */
void die(const char *s) {
    endwin();
    perror(s);
    exit(1);
}

/* Free operation memory */
void free_operation(operation *op) {
    if (op->type == OP_INSERT_LINE || op->type == OP_DELETE_LINE) {
        if (op->line) free(op->line);
    }
    free(op);
}

/* Free operations stack */
void free_operations_stack(operation *stack) {
    operation *current = stack;
    while (current != NULL) {
        operation *next = current->next;
        free_operation(current);
        current = next;
    }
}

/* Initialize the editor */
void init_editor() {
    /* Clear all memory first */
    memset(&E, 0, sizeof(E));
    /* Initialize command buffer */
    E.commandbuf[0] = '\0';
    E.commandlen = 0;
    
    /* Initialize cursor and screen positions */
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.mode = MODE_NORMAL;
    
    /* Initialize selection */
    E.sel_start_x = E.sel_start_y = -1;
    E.sel_end_x = E.sel_end_y = -1;
    E.selecting = 0;
    
    /* Initialize display options */
    E.show_line_numbers = 0;  /* Line numbers off by default */
    
    /* Initialize clipboard */
    E.clipboard = NULL;
    E.clipboard_len = 0;
    
    /* Initialize font size (3 = normal) */
    E.font_size = 3;
    
    /* Set up terminal */
    /* Enable color if supported */
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_WHITE, COLOR_BLACK);     /* Normal text */
        init_pair(2, COLOR_BLACK, COLOR_WHITE);     /* Selected text */
        init_pair(3, COLOR_GREEN, COLOR_BLACK);     /* Status bar */
        init_pair(4, COLOR_YELLOW, COLOR_BLACK);    /* Line numbers */
    }
    
    /* Get screen size */
    getmaxyx(stdscr, E.screenrows, E.screencols);
    
    /* Make sure we have enough rows for status bar and command line */
    if (E.screenrows < 3) {
        /* If terminal is too small, set minimum usable size */
        E.screenrows = 1;
    } else {
        /* Reserve bottom 2 rows for status bar and command line */
        E.screenrows -= 2;
    }
    
    /* Ensure we have minimum screen dimensions */
    if (E.screenrows < 3) E.screenrows = 3;
    if (E.screencols < 20) E.screencols = 20;
    
    /* Initialize rows */
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.dirty = 0;
    
    /* Initialize mode */
    E.mode = MODE_NORMAL;
    
    /* Initialize command buffer */
    E.commandbuf[0] = '\0';
    E.commandlen = 0;
    
    /* Initialize colors if terminal supports them */
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_WHITE, COLOR_BLACK);   /* Normal text */
        init_pair(2, COLOR_BLACK, COLOR_WHITE);   /* Selected text */
        init_pair(3, COLOR_BLACK, COLOR_CYAN);    /* Status bar */
        init_pair(4, COLOR_CYAN, COLOR_BLACK);    /* Line numbers */
    }
    
    cbreak();           /* Disable line buffering */
    keypad(stdscr, 1); /* Enable keypad */
    mouseinterval(0);  /* Disable mouse click resolution delay */
    
    /* Welcome message */
    snprintf(E.statusmsg, sizeof(E.statusmsg), 
             "HELP: cc = insert | Ctrl+Z = undo | Ctrl+Y = redo | Ctrl+A = select all");
}

/* Push operation to stack */
void push_operation(operation **stack, enum operation_type type, int cx, int cy, char c, char *line, int line_size) {
    operation *op = malloc(sizeof(operation));
    op->type = type;
    op->cx = cx;
    op->cy = cy;
    op->c = c;
    
    if (line && line_size > 0) {
        op->line = malloc(line_size + 1);
        memcpy(op->line, line, line_size);
        op->line[line_size] = '\0';
    } else {
        op->line = NULL;
    }
    
    op->line_size = line_size;
    op->next = *stack;
    *stack = op;
}

/* Insert a row at the specified position */
void editor_insert_row(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    
    /* Allocate memory for new row */
    erow *new_rows = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    if (new_rows == NULL) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Memory allocation failed");
        return;
    }
    E.row = new_rows;
    
    /* Move existing rows */
    if (at < E.numrows) {
        memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    }
    
    /* Allocate and initialize new row */
    E.row[at].chars = malloc(len + 1);
    if (E.row[at].chars == NULL) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Memory allocation failed");
        return;
    }
    
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].size = len;
    E.numrows++;
    E.dirty++;
    
    /* Update status message */
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Line inserted at position %d", at + 1);
    
    /* Add to undo stack */
    push_operation(&E.undo_stack, OP_INSERT_LINE, 0, at, 0, s, len);
    free_operations_stack(E.redo_stack);
    E.redo_stack = NULL;
}

/* Free row memory */
void editor_free_row(erow *row) {
    free(row->chars);
}

/* Delete a row */
void editor_del_row(int at) {
    if (at < 0 || at >= E.numrows) return;
    
    /* Add to undo stack before deleting */
    push_operation(&E.undo_stack, OP_DELETE_LINE, 0, at, 0, E.row[at].chars, E.row[at].size);
    
    editor_free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
    
    free_operations_stack(E.redo_stack);
    E.redo_stack = NULL;
}

/* Convert row and column to file position */
int editor_row_cx_to_rx(erow *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (8 - 1) - (rx % 8);
        rx++;
    }
    return rx;
}

/* Insert character at current position */
void editor_insert_char(int c) {
    if (E.cy == E.numrows) {
        editor_insert_row(E.numrows, "", 0);
    }
    erow *row = &E.row[E.cy];
    
    /* Add to undo stack */
    push_operation(&E.undo_stack, OP_INSERT_CHAR, E.cx, E.cy, c, NULL, 0);
    
    char *new_buf = realloc(row->chars, row->size + 2);
    if (new_buf == NULL) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Memory allocation failed");
        return;
    }
    row->chars = new_buf;
    memmove(&row->chars[E.cx + 1], &row->chars[E.cx], row->size - E.cx + 1);
    row->size++;
    row->chars[E.cx] = c;
    E.cx++;
    E.dirty++;
    
    free_operations_stack(E.redo_stack);
    E.redo_stack = NULL;
}

/* Insert newline */
void editor_insert_newline() {
    /* Add to undo stack - we need to handle this before modifying anything */
    free_operations_stack(E.redo_stack);
    E.redo_stack = NULL;
    
    /* Handle case where there are no rows yet */
    if (E.numrows == 0) {
        /* Create a new empty row */
        editor_insert_row(0, "", 0);
        /* Add to undo stack */
        push_operation(&E.undo_stack, OP_NEWLINE, 0, 0, '\n', NULL, 0);
        E.cy = 0;
        E.cx = 0;
        E.dirty++;
        return;
    }
    
    /* Save the current row content for undo */
    char *line_copy = NULL;
    int line_size = 0;
    
    if (E.cx < E.row[E.cy].size) {
        line_size = E.row[E.cy].size - E.cx;
        line_copy = malloc(line_size + 1);
        if (line_copy) {
            memcpy(line_copy, &E.row[E.cy].chars[E.cx], line_size);
            line_copy[line_size] = '\0';
        }
    }
    
    /* Insert the new row */
    if (E.cx == 0) {
        editor_insert_row(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy]; /* Re-get the pointer as it might have changed */
        row->size = E.cx;
        row->chars[row->size] = '\0';
    }
    
    /* Add to undo stack */
    push_operation(&E.undo_stack, OP_NEWLINE, E.cx, E.cy, '\n', line_copy, line_size);
    
    /* Update cursor position */
    E.cy++;
    E.cx = 0;
    E.dirty++;
    
    /* Free the line copy if it was allocated */
    if (line_copy) {
        free(line_copy);
    }
}

/* Delete character at cursor */
void editor_del_char() {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        /* Add to undo stack */
        push_operation(&E.undo_stack, OP_DELETE_CHAR, E.cx - 1, E.cy, row->chars[E.cx - 1], NULL, 0);
        
        memmove(&row->chars[E.cx - 1], &row->chars[E.cx], row->size - E.cx + 1);
        E.cx--;
        row->size--;
        E.dirty++;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editor_insert_row(E.cy - 1, row->chars, row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
    
    free_operations_stack(E.redo_stack);
    E.redo_stack = NULL;
}

/* Undo last operation */
void editor_undo() {
    if (E.undo_stack == NULL) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Nothing to undo");
        return;
    }
    
    operation *op = E.undo_stack;
    E.undo_stack = op->next;
    
    switch (op->type) {
        case OP_INSERT_CHAR:
            /* To undo an insert, delete the character */
            E.cx = op->cx;
            E.cy = op->cy;
            if (E.cy < E.numrows) {
                erow *row = &E.row[E.cy];
                if (E.cx < row->size) {
                    memmove(&row->chars[E.cx], &row->chars[E.cx + 1], row->size - E.cx);
                    row->size--;
                    E.dirty++;
                }
            }
            break;
            
        case OP_DELETE_CHAR:
            /* To undo a delete, insert the character */
            E.cx = op->cx;
            E.cy = op->cy;
            if (E.cy < E.numrows) {
                erow *row = &E.row[E.cy];
                char *new_buf = realloc(row->chars, row->size + 2);
                if (new_buf == NULL) {
                    snprintf(E.statusmsg, sizeof(E.statusmsg), "Memory allocation failed");
                    return;
                }
                row->chars = new_buf;
                memmove(&row->chars[E.cx + 1], &row->chars[E.cx], row->size - E.cx + 1);
                row->size++;
                row->chars[E.cx] = op->c;
                E.dirty++;
            }
            break;
            
        case OP_INSERT_LINE:
            /* To undo an inserted line, delete it */
            editor_del_row(op->cy);
            E.cx = 0;
            E.cy = op->cy;
            break;
            
        case OP_DELETE_LINE:
            /* To undo a deleted line, insert it back */
            editor_insert_row(op->cy, op->line, op->line_size);
            E.cx = 0;
            E.cy = op->cy;
            break;
            
        case OP_NEWLINE:
            /* To undo a newline, we need to merge the current line with the previous one */
            if (E.cy > 0) {
                erow *prev_row = &E.row[E.cy - 1];
                erow *curr_row = &E.row[E.cy];
                
                /* Save the original line content for redo */
                char *line_copy = NULL;
                if (op->line) {
                    line_copy = strdup(op->line);
                }
                
                /* Merge the current line into the previous one */
                int new_size = prev_row->size + curr_row->size;
                char *new_buf = realloc(prev_row->chars, new_size + 1);
                if (new_buf) {
                    prev_row->chars = new_buf;
                    memcpy(prev_row->chars + prev_row->size, curr_row->chars, curr_row->size);
                    prev_row->size = new_size;
                    prev_row->chars[new_size] = '\0';
                    
                    /* Delete the current row */
                    editor_del_row(E.cy);
                    
                    /* Update cursor position */
                    E.cy--;
                    E.cx = op->cx;
                    E.dirty++;
                    
                    /* Update the operation for redo */
                    free(op->line);
                    op->line = line_copy;
                    if (line_copy) {
                        op->line_size = strlen(line_copy);
                    } else {
                        op->line_size = 0;
                    }
                }
            }
            break;
    }
    
    /* Add to redo stack */
    push_operation(&E.redo_stack, op->type, op->cx, op->cy, op->c, op->line, op->line_size);
    
    free_operation(op);
}

/* Redo last undone operation */
void editor_redo() {
    if (E.redo_stack == NULL) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Nothing to redo");
        return;
    }
    
    operation *op = E.redo_stack;
    E.redo_stack = op->next;
    
    switch (op->type) {
        case OP_INSERT_CHAR:
            /* To redo an insert, insert the character again */
            E.cx = op->cx;
            E.cy = op->cy;
            editor_insert_char(op->c);
            break;
            
        case OP_DELETE_CHAR:
            /* To redo a delete, delete the character again */
            E.cx = op->cx + 1;
            E.cy = op->cy;
            editor_del_char();
            break;
            
        case OP_INSERT_LINE:
            /* To redo an inserted line, insert it again */
            editor_insert_row(op->cy, op->line, op->line_size);
            E.cx = 0;
            E.cy = op->cy;
            break;
            
        case OP_DELETE_LINE:
            /* To redo a deleted line, delete it again */
            editor_del_row(op->cy);
            E.cx = 0;
            E.cy = op->cy;
            break;
            
        case OP_NEWLINE:
            /* To redo a newline, we need to split the line at the cursor position */
            if (E.cy < E.numrows) {
                /* Save the current cursor position */
                int save_cx = E.cx;
                int save_cy = E.cy;
                
                /* Set cursor to the position where newline was inserted */
                E.cx = op->cx;
                E.cy = op->cy;
                
                /* Insert a newline at the cursor position */
                editor_insert_newline();
                
                /* If there was text after the cursor, restore it to the new line */
                if (op->line && op->line_size > 0) {
                    erow *new_row = &E.row[E.cy];
                    free(new_row->chars);
                    new_row->chars = malloc(op->line_size + 1);
                    if (new_row->chars) {
                        memcpy(new_row->chars, op->line, op->line_size);
                        new_row->size = op->line_size;
                        new_row->chars[op->line_size] = '\0';
                    } else {
                        new_row->size = 0;
                    }
                }
                
                /* Restore cursor position */
                E.cx = save_cx;
                E.cy = save_cy;
            }
            break;
    }
    
    /* Add back to undo stack */
    push_operation(&E.undo_stack, op->type, op->cx, op->cy, op->c, op->line, op->line_size);
    
    free_operation(op);
}

/* Start selection at current cursor position */
void editor_selection_start() {
    E.sel_start_x = E.cx;
    E.sel_start_y = E.cy;
    E.sel_end_x = E.cx;
    E.sel_end_y = E.cy;
    E.selecting = 1;
}

/* Update selection end point to current cursor position */
void editor_selection_update() {
    if (E.selecting) {
        E.sel_end_x = E.cx;
        E.sel_end_y = E.cy;
    }
}

/* Clear selection */
void editor_selection_clear() {
    E.sel_start_x = -1;
    E.sel_start_y = -1;
    E.sel_end_x = -1;
    E.sel_end_y = -1;
    E.selecting = 0;
}

/* Normalize selection (ensure start comes before end) */
void editor_selection_normalize() {
    /* Swap if end is before start */
    if (E.sel_end_y < E.sel_start_y || 
        (E.sel_end_y == E.sel_start_y && E.sel_end_x < E.sel_start_x)) {
        int temp_x = E.sel_start_x;
        int temp_y = E.sel_start_y;
        E.sel_start_x = E.sel_end_x;
        E.sel_start_y = E.sel_end_y;
        E.sel_end_x = temp_x;
        E.sel_end_y = temp_y;
    }
}

/* Copy selected text to clipboard */
void editor_copy_selection() {
    /* Check if selection exists */
    if (E.sel_start_x == -1 || E.sel_start_y == -1 || 
        E.sel_end_x == -1 || E.sel_end_y == -1) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "No selection to copy");
        return;
    }
    
    /* Normalize selection */
    editor_selection_normalize();
    
    /* Free previous clipboard content */
    if (E.clipboard) {
        free(E.clipboard);
        E.clipboard = NULL;
    }
    
    /* Allocate clipboard for selected lines */
    int num_lines = E.sel_end_y - E.sel_start_y + 1;
    if (num_lines <= 0) {
        E.clipboard = NULL;
        E.clipboard_len = 0;
        return;
    }
    
    E.clipboard = malloc(num_lines * sizeof(char *));
    if (!E.clipboard) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Error: Out of memory");
        E.clipboard_len = 0;
        return;
    }
    E.clipboard_len = num_lines;
    
    if (num_lines == 1) {
        /* Single line selection */
        erow *row = &E.row[E.sel_start_y];
        int len = E.sel_end_x - E.sel_start_x;
        E.clipboard[0] = malloc(len + 1);
        memcpy(E.clipboard[0], &row->chars[E.sel_start_x], len);
        E.clipboard[0][len] = '\0';
    } else {
        /* Multi-line selection */
        for (int i = 0; i < num_lines; i++) {
            if (E.sel_start_y + i >= E.numrows) break;
            
            erow *row = &E.row[E.sel_start_y + i];
            int start = (i == 0) ? E.sel_start_x : 0;
            int end = (i == num_lines - 1) ? E.sel_end_x : row->size;
            int len = end - start;
            
            E.clipboard[i] = malloc(len + 1);
            memcpy(E.clipboard[i], &row->chars[start], len);
            E.clipboard[i][len] = '\0';
        }
    }
    
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Copied %d lines", num_lines);
}

/* Paste clipboard at current position */
void editor_paste() {
    if (E.clipboard == NULL || E.clipboard_len == 0) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Nothing to paste");
        return;
    }
    
    if (E.clipboard_len == 1) {
        /* Single line paste */
        char *line = E.clipboard[0];
        for (int i = 0; line[i] != '\0'; i++) {
            editor_insert_char(line[i]);
        }
    } else {
        /* Multi-line paste */
        for (int i = 0; i < E.clipboard_len; i++) {
            char *line = E.clipboard[i];
            for (int j = 0; line[j] != '\0'; j++) {
                editor_insert_char(line[j]);
            }
            if (i < E.clipboard_len - 1) {
                editor_insert_newline();
            }
        }
    }
    
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Pasted %d lines", E.clipboard_len);
}

/* Select all text */
void editor_select_all() {
    if (E.numrows > 0) {
        E.sel_start_x = 0;
        E.sel_start_y = 0;
        E.sel_end_y = E.numrows - 1;
        E.sel_end_x = E.row[E.numrows - 1].size;
        E.selecting = 1;
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Selected all text");
    }
}

/* Change font size */
void editor_change_font_size(int delta) {
    E.font_size += delta;
    if (E.font_size < 1) E.font_size = 1;
    if (E.font_size > 5) E.font_size = 5;
    
    snprintf(E.statusmsg, sizeof(E.statusmsg), "Font size: %d", E.font_size);
    
    /* Note: ncurses doesn't actually support font size change,
       this is more of a placeholder for graphical terminals */
}

/* File I/O */
void editor_open(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);
    if (!E.filename) {
        die("strdup failed");
    }

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        /* New file */
        return;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    
    /* Use our own implementation of getline if not available */
    #if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
    #define BUF_SIZE 1024
    char buffer[BUF_SIZE];
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        size_t len = strlen(buffer);
        /* Remove trailing newline if present */
        if (len > 0 && (buffer[len-1] == '\n' || buffer[len-1] == '\r')) {
            buffer[--len] = '\0';
            /* Handle CRLF if present */
            if (len > 0 && buffer[len-1] == '\r') {
                buffer[--len] = '\0';
            }
        }
        /* Allocate or reallocate line buffer */
        if (line == NULL) {
            line = malloc(len + 1);
            if (!line) die("malloc failed");
            strcpy(line, buffer);
        } else {
            size_t old_len = strlen(line);
            char *new_line = realloc(line, old_len + len + 1);
            if (!new_line) {
                free(line);
                die("realloc failed");
            }
            line = new_line;
            strcat(line, buffer);
        }
        linelen = strlen(line);
        
        /* Process the line */
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_insert_row(E.numrows, line, linelen);
        
        /* Reset line for next iteration */
        free(line);
        line = NULL;
    }
    #else
    /* Use system's getline */
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_insert_row(E.numrows, line, linelen);
    }
    free(line);
    #endif
    
    fclose(fp);
    E.dirty = 0;
    
    /* Clear undo/redo stacks when opening a file */
    free_operations_stack(E.undo_stack);
    E.undo_stack = NULL;
    free_operations_stack(E.redo_stack);
    E.redo_stack = NULL;
}

/* Save the current file */
int editor_save() {
    if (E.filename == NULL) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Error: No filename");
        return -1;
    }

    FILE *fp = fopen(E.filename, "w");
    if (!fp) {
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Can't save! I/O error: %s", strerror(errno));
        return -1;
    }

    int i;
    for (i = 0; i < E.numrows; i++) {
        fwrite(E.row[i].chars, 1, E.row[i].size, fp);
        fwrite("\n", 1, 1, fp);
    }

    fclose(fp);
    E.dirty = 0;
    snprintf(E.statusmsg, sizeof(E.statusmsg), "%d lines written to %s", E.numrows, E.filename);
    return 0;
}

/* Process command with optional double colon prefix.
 * Normalizes the command to start with exactly one colon.
 * Handles cases like ':', '::', '::cmd', 'cmd' etc.
 * Returns 1 if the command is valid, 0 if it should be ignored.
 */
int process_command_prefix(char *cmd) {
    if (!cmd || !*cmd) return 0;  /* NULL or empty command */
    
    size_t len = strlen(cmd);
    if (len >= 256) return 0;  /* Command too long */
    
    /* Skip leading whitespace */
    char *p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    if (p > cmd) {
        memmove(cmd, p, len - (p - cmd) + 1);
        len = strlen(cmd);
    }
    
    /* Find first non-colon character */
    size_t first_non_colon = 0;
    while (first_non_colon < len && cmd[first_non_colon] == ':') {
        first_non_colon++;
    }
    
    /* Handle empty command or just colons */
    if (first_non_colon >= len) {
        cmd[0] = ':';
        cmd[1] = '\0';
        return 0;  /* Don't process empty commands */
    }
    
    /* If command doesn't start with a colon, add one */
    if (first_non_colon == 0) {
        if (len + 1 >= sizeof(E.commandbuf)) return 0;  /* Check buffer space */
        memmove(cmd + 1, cmd, len + 1);  /* +1 for null terminator */
        cmd[0] = ':';
        len++;
    }
    /* If command starts with multiple colons, collapse them to one */
    else if (first_non_colon > 1) {
        memmove(cmd + 1, cmd + first_non_colon, len - first_non_colon + 1);
        cmd[0] = ':';
        len -= (first_non_colon - 1);
    }
    
    /* Trim any whitespace after the colon */
    p = cmd + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (p > cmd + 1) {
        memmove(cmd + 1, p, len - (p - cmd) + 1);
    }
    
    /* Ensure the command is not just a colon */
    return (strlen(cmd) > 1);
}

/* Process command */
int editor_process_command() {
    /* Ensure command is null-terminated */
    size_t cmd_buf_size = sizeof(E.commandbuf);
    if (E.commandlen >= 0 && (size_t)E.commandlen >= cmd_buf_size) {
        E.commandbuf[cmd_buf_size - 1] = '\0';
        E.commandlen = (int)(cmd_buf_size - 1);
    } else {
        E.commandbuf[E.commandlen] = '\0';
    }
    
    /* Process command prefix (handle double colons and whitespace) */
    if (!process_command_prefix(E.commandbuf)) {
        /* Empty or invalid command */
        return 0;
    }
    E.commandlen = strlen(E.commandbuf);
    
    /* Command history feature */
    static char cmd_history[10][256];  /* Store last 10 commands */
    static int cmd_history_pos = 0;
    static int cmd_history_len = 0;
    
    /* Save current cursor and scroll position to restore after command */
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_rowoff = E.rowoff;
    int saved_coloff = E.coloff;
    
    /* Parse command */
    int should_quit = 0;
    int force_quit = 0;
    int preserve_position = 1;  /* By default, preserve cursor position */
    
    /* Trim any trailing whitespace from command */
    char *cmd = E.commandbuf;
    while (*cmd == ' ' || *cmd == '\t') cmd++;  /* Skip leading whitespace */
    char *end = cmd + strlen(cmd) - 1;
    while (end > cmd && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end-- = '\0';
    }
    
    /* Check for vim-like commands */
    if (strcmp(cmd, ":q") == 0 || strcmp(cmd, ":quit") == 0 ||
        strcmp(cmd, "::q") == 0 || strcmp(cmd, "::quit") == 0) {
        /* Quit if not dirty */
        if (E.dirty) {
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                    "No write since last change (add ! to override)");
        } else {
            should_quit = 1;
        }
    } else if (strcmp(cmd, ":q!") == 0 || strcmp(cmd, ":quit!") == 0 ||
               strcmp(cmd, "::q!") == 0 || strcmp(cmd, "::quit!") == 0) {
        /* Force quit without saving */
        should_quit = 1;
        force_quit = 1;
    } else if (strcmp(cmd, ":w") == 0 || strcmp(cmd, "::w") == 0) {
        /* Save file */
        editor_save();
    } else if (strcmp(cmd, ":wq") == 0 || strcmp(cmd, "::wq") == 0 ||
               strcmp(cmd, ":sq") == 0 || strcmp(cmd, "::sq") == 0) {
        /* Save and quit */
        if (editor_save() == 0) {
            should_quit = 1;
        }
    } else if (strncmp(cmd, ":e ", 3) == 0 || strncmp(cmd, "::e ", 4) == 0) {
        /* Edit file - open a new file */
        char *filename = cmd + (cmd[1] == ':' ? 4 : 3);
        if (E.dirty) {
            snprintf(E.statusmsg, sizeof(E.statusmsg),
                    "No write since last change (add ! to override)");
        } else {
            /* Clear editor content */
            for (int i = E.numrows - 1; i >= 0; i--) {
                editor_del_row(i);
            }
            editor_open(filename);
            snprintf(E.statusmsg, sizeof(E.statusmsg), "Opened %s", filename);
            preserve_position = 0;  /* Don't preserve position when opening new file */
        }
    } else if (strncmp(cmd, ":e! ", 4) == 0 || strncmp(cmd, "::e! ", 5) == 0) {
        /* Force edit file - open a new file without saving */
        char *filename = cmd + (cmd[1] == ':' ? 5 : 4);
        /* Clear editor content */
        for (int i = E.numrows - 1; i >= 0; i--) {
            editor_del_row(i);
        }
        editor_open(filename);
        snprintf(E.statusmsg, sizeof(E.statusmsg), "Opened %s", filename);
        preserve_position = 0;  /* Don't preserve position when opening new file */
    } else {
        /* Limit command display to avoid buffer overflow */
        char cmd_display[60];
        strncpy(cmd_display, cmd, sizeof(cmd_display) - 1);
        cmd_display[sizeof(cmd_display) - 1] = '\0';
        
        snprintf(E.statusmsg, sizeof(E.statusmsg),
                "Unknown command: %s", cmd_display);
    }
    
    /* Save command in history if not empty */
    if (E.commandlen > 1) {
        strncpy(cmd_history[cmd_history_pos], E.commandbuf, 255);
        cmd_history[cmd_history_pos][255] = '\0';
        cmd_history_pos = (cmd_history_pos + 1) % 10;
        if (cmd_history_len < 10) cmd_history_len++;
    }
    
    /* Restore cursor and scroll position if needed */
    if (preserve_position && !should_quit) {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.rowoff = saved_rowoff;
        E.coloff = saved_coloff;
    }
    
    /* Handle quit command */
    if (should_quit) {
        if (force_quit || !E.dirty) {
            editor_cleanup();
            exit(0);
        }
    }
    
    return 1;
}

/* Scroll the editor if cursor moves out of the visible window */
void editor_scroll() {
    /* Vertical scrolling */
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    /* Horizontal scrolling */
    if (E.cx < E.coloff) {
        E.coloff = E.cx;
    }
    if (E.cx >= E.coloff + E.screencols) {
        E.coloff = E.cx - E.screencols + 1;
    }
    
    /* Ensure offsets are never negative */
    if (E.rowoff < 0) E.rowoff = 0;
    if (E.coloff < 0) E.coloff = 0;
}

/* Check if position is within selection */
int is_position_selected(int x, int y) {
    if (!E.selecting || E.sel_start_x == -1) return 0;
    
    /* Normalize selection */
    int start_x, start_y, end_x, end_y;
    if (E.sel_end_y < E.sel_start_y || 
        (E.sel_end_y == E.sel_start_y && E.sel_end_x < E.sel_start_x)) {
        start_x = E.sel_end_x;
        start_y = E.sel_end_y;
        end_x = E.sel_start_x;
        end_y = E.sel_start_y;
    } else {
        start_x = E.sel_start_x;
        start_y = E.sel_start_y;
        end_x = E.sel_end_x;
        end_y = E.sel_end_y;
    }
    
    if (y < start_y || y > end_y) return 0;
    if (y == start_y && x < start_x) return 0;
    if (y == end_y && x >= end_x) return 0;
    
    return 1;
}

/* Draw the editor rows */
void editor_draw_rows() {
    int y;
    char line_num[10];  /* Buffer for line numbers */
    int line_num_width = E.show_line_numbers ? 4 : 0;  /* Width of line number display */
    
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        
        /* Draw line numbers if enabled and we have content */
        if (E.show_line_numbers && filerow < E.numrows) {
            attron(COLOR_PAIR(4));  /* Line number color */
            
            /* Format line number to fit in buffer */
            /* Use a safe, fixed-width buffer for line number display */
            if (filerow + 1 > 0 && filerow + 1 < 1000) {
                /* Up to 3 digits */
                snprintf(line_num, sizeof(line_num), "%3d ", filerow + 1);
            } else if (filerow + 1 >= 1000 && filerow + 1 < 1000000) {
                /* 4-6 digits, show as e.g. '999k' */
                snprintf(line_num, sizeof(line_num), "%3dk", (filerow + 1) / 1000);
            } else if (filerow + 1 >= 1000000) {
                /* 7+ digits, show as '***' */
                snprintf(line_num, sizeof(line_num), "***");
            } else {
                /* Negative or invalid row */
                snprintf(line_num, sizeof(line_num), "???");
            }
            
            mvprintw(y, 0, "%s", line_num);
            attroff(COLOR_PAIR(4));
        }
        
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "ABC Vi -- version 0.0.3");
                if (welcomelen > E.screencols - line_num_width) 
                    welcomelen = E.screencols - line_num_width;
                int padding = (E.screencols - line_num_width - welcomelen) / 2;
                if (padding) {
                    mvaddch(y, line_num_width, '~');
                    padding--;
                }
                attron(COLOR_PAIR(1));  /* Normal text color */
                mvprintw(y, line_num_width + padding + 1, "%s", welcome);
                attroff(COLOR_PAIR(1));
            } else {
                mvaddch(y, line_num_width, '~');
            }
        } else {
            int len = E.row[filerow].size - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols - line_num_width) 
                len = E.screencols - line_num_width;
            
            /* Print the line character by character with syntax highlighting */
            attron(COLOR_PAIR(1));  /* Normal text color */
            for (int i = 0; i < len; i++) {
                if (E.coloff + i < E.row[filerow].size) {
                    int c = E.row[filerow].chars[E.coloff + i] & 0xff;
                    if (is_position_selected(E.coloff + i, filerow)) {
                        attron(COLOR_PAIR(2));  /* Selected text color */
                        mvaddch(y, i + line_num_width, c);
                        attroff(COLOR_PAIR(2));
                    } else {
                        mvaddch(y, i + line_num_width, c);
                    }
                }
            }
            attroff(COLOR_PAIR(1));
        }
        clrtoeol();
    }
}

/* Draw the status bar */
void editor_draw_status_bar() {
    /* Use color pair for status bar if colors are supported */
    if (has_colors()) {
        attron(COLOR_PAIR(3));  /* Status bar color */
    } else {
        attron(A_REVERSE);
    }

    /* Left status */
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");
    
    /* Right status with enhanced info */
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %dx%d | %d:%d | %d%%",
        E.mode == MODE_NORMAL ? "NORMAL" : 
        E.mode == MODE_INSERT ? "INSERT" :
        E.mode == MODE_SELECTION ? "SELECT" : "COMMAND",
        E.screencols, E.screenrows,
        E.cy + 1, E.cx + 1,
        E.numrows ? (E.cy * 100) / E.numrows : 0);
    
    /* Ensure status fits within screen */
    if (len > E.screencols) len = E.screencols;
    mvprintw(E.screenrows, 0, "%s", status);
    
    /* Fill middle space */
    int space_left = E.screencols - len - rlen;
    if (space_left > 0) {
        for (int i = 0; i < space_left; i++) {
            mvaddch(E.screenrows, len + i, ' ');
        }
    }
    
    /* Print right status if there's room */
    if (E.screencols - len >= rlen) {
        mvprintw(E.screenrows, E.screencols - rlen, "%s", rstatus);
    }
    
    /* Reset attributes */
    if (has_colors()) {
        attroff(COLOR_PAIR(3));
    } else {
        attroff(A_REVERSE);
    }
}

/* Draw the command line */
void editor_draw_command_line() {
    /* Get terminal dimensions */
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    /* Check if we have space for command line */
    if (E.screenrows + 1 >= max_y) {
        return;  /* Not enough space */
    }
    
    /* Clear the command line area */
    move(E.screenrows + 1, 0);
    clrtoeol();
    
    if (E.mode == MODE_COMMAND) {
        /* Ensure command buffer is properly terminated */
        if (E.commandlen < 0) E.commandlen = 0;
        if (E.commandlen >= (int)sizeof(E.commandbuf)) {
            E.commandlen = (int)sizeof(E.commandbuf) - 1;
        }
        E.commandbuf[E.commandlen] = '\0';  /* Always null-terminate */
        
        /* Draw command prompt */
        attron(COLOR_PAIR(1) | A_BOLD);
        mvaddch(E.screenrows + 1, 0, ':');
        
        /* Calculate available space for command */
        int available_width = max_x - 1;  /* -1 for the colon */
        if (available_width < 1) available_width = 1;
        
        /* Print command content - only printable ASCII characters */
        for (int i = 0; i < available_width && i < E.commandlen; i++) {
            unsigned char c = (unsigned char)E.commandbuf[i];
            /* Only display printable ASCII characters (32-126) */
            if (c >= 32 && c <= 126) {
                /* Special handling for colon - only show one at the beginning */
                if (c == ':' && i == 0) {
                    /* Already displayed by the prompt */
                    continue;
                }
                mvaddch(E.screenrows + 1, i + 1, c);
            } else {
                /* Skip non-printable characters in display */
                mvaddch(E.screenrows + 1, i + 1, ' ');
            }
        }
        
        /* Clear any remaining space in the command line */
        for (int i = E.commandlen + 1; i <= available_width; i++) {
            mvaddch(E.screenrows + 1, i, ' ');
        }
        
        /* Position cursor */
        int cursor_pos = E.commandlen + 1;
        if (cursor_pos > available_width) cursor_pos = available_width;
        move(E.screenrows + 1, cursor_pos);
        
        attroff(COLOR_PAIR(1) | A_BOLD);
    } else {
        /* Show status message with timeout */
        static time_t last_status_time = 0;
        static const int STATUS_TIMEOUT = 5;  /* 5 seconds */
        
        time_t current_time = time(NULL);
        if (E.statusmsg[0] != '\0' && 
            current_time - last_status_time < STATUS_TIMEOUT) {
            
            /* Use different colors for different message types */
            if (strstr(E.statusmsg, "Error") == E.statusmsg) {
                attron(COLOR_PAIR(2));  /* Error messages */
            } else if (strstr(E.statusmsg, "Warning") == E.statusmsg) {
                attron(COLOR_PAIR(4));  /* Warning messages */
            } else {
                attron(COLOR_PAIR(3));  /* Normal messages */
            }
            
            mvprintw(E.screenrows + 1, 0, "%.256s", E.statusmsg);
            attroff(COLOR_PAIR(2) | COLOR_PAIR(3) | COLOR_PAIR(4));
        } else {
            E.statusmsg[0] = '\0';  /* Clear old messages */
        }
        
        /* Update last status time when new message is set */
        if (E.statusmsg[0] != '\0') {
            last_status_time = current_time;
        }
    }
}

/* Refresh the screen with current editor content */
void editor_refresh_screen() {
    /* Save current cursor position */
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    
    /* Check if terminal size has changed */
    int current_rows, current_cols;
    getmaxyx(stdscr, current_rows, current_cols);
    if (current_rows != E.screenrows + 2 || current_cols != E.screencols) {
        /* Update editor dimensions */
        if (current_rows < 3) {
            /* If terminal is too small, set minimum usable size */
            E.screenrows = 1;
        } else {
            /* Reserve bottom 2 rows for status bar and command line */
            E.screenrows = current_rows - 2;
        }
        E.screencols = current_cols;
        if (E.screencols < 20) E.screencols = 20;
    }
    
    editor_scroll();
    
    /* Use erase instead of clear for better performance */
    erase();
    
    /* Handle screen redraw */
    editor_draw_rows();
    editor_draw_status_bar();
    editor_draw_command_line();
    
    /* Position cursor */
    if (E.mode == MODE_COMMAND) {
        /* Position cursor in command line */
        move(E.screenrows + 1, E.commandlen + 1);  /* +1 for the colon */
    } else {
        /* Calculate screen coordinates */
        int screen_y = saved_cy - E.rowoff;
        int screen_x = saved_cx - E.coloff;
        
        /* Ensure cursor stays within visible screen bounds */
        if (screen_y >= 0 && screen_y < E.screenrows && 
            screen_x >= 0 && screen_x < E.screencols) {
            move(screen_y, screen_x);
        } else {
            /* If cursor would be outside visible area, place it at a valid position */
            if (screen_y < 0) screen_y = 0;
            if (screen_y >= E.screenrows) screen_y = E.screenrows - 1;
            if (screen_x < 0) screen_x = 0;
            if (screen_x >= E.screencols) screen_x = E.screencols - 1;
            move(screen_y, screen_x);
        }
    }
    
    /* Force screen update */
    refresh();
}

/* Move cursor */
/* Completing the editor_move_cursor function */
void editor_move_cursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    
    switch (key) {
        case KEY_LEFT:
        case 'h':
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                /* Move to end of previous line */
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case KEY_RIGHT:
        case 'l':
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size && E.cy < E.numrows - 1) {
                /* Move to beginning of next line */
                E.cy++;
                E.cx = 0;
            }
            break;
        case KEY_UP:
        case 'k':
            if (E.cy > 0) {
                E.cy--;
                /* Adjust horizontal position if needed */
                if (E.cx > E.row[E.cy].size)
                    E.cx = E.row[E.cy].size;
            }
            break;
        case KEY_DOWN:
        case 'j':
            if (E.cy < E.numrows - 1) {
                E.cy++;
                /* Adjust horizontal position if needed */
                if (E.cx > E.row[E.cy].size)
                    E.cx = E.row[E.cy].size;
            }
            break;
        case KEY_HOME:
        case '0':
            E.cx = 0;
            break;
        case KEY_END:
        case '$':
            if (row) E.cx = row->size;
            break;
        case KEY_PPAGE:  /* Page Up */
            {
                E.cy = E.rowoff;
                int times = E.screenrows;
                while (times--) {
                    if (E.cy > 0) E.cy--;
                }
                /* Adjust cursor position if needed */
                if (E.cy < E.numrows && E.cx > E.row[E.cy].size) {
                    E.cx = E.row[E.cy].size;
                }
            }
            break;
        case KEY_NPAGE:  /* Page Down */
            {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy >= E.numrows) E.cy = E.numrows - 1;
                int times = E.screenrows;
                while (times--) {
                    if (E.cy < E.numrows - 1) E.cy++;
                }
                /* Adjust cursor position if needed */
                if (E.cy < E.numrows && E.cx > E.row[E.cy].size) {
                    E.cx = E.row[E.cy].size;
                }
            }
            break;
    }
    
    /* Update selection if in selection mode */
    if (E.mode == MODE_SELECTION) {
        editor_selection_update();
    }
}

/* Process keyboard input */
void editor_process_keypress() {
    int c = getch();
    
    /* Debug key code if needed */
    /*
    char debug[80];
    snprintf(debug, sizeof(debug), "Key: %d (0x%x)", c, c);
    mvprintw(0, 0, "%s", debug);
    refresh();
    */
    
    /* Ctrl-Shift-Q (Quit): works in all modes */
    /* ncurses/PDCurses does not have a perfect portable code for Ctrl-Shift-Q,
       but often Ctrl-Shift-Q is 17 (ASCII DC1, ^Q). Adjust if your terminal gives another code. */
    if (c == 17) { /* Ctrl-Shift-Q or Ctrl-Q */
        editor_cleanup();
        exit(0);
    }

    /* Handle special keys for copy/paste/help */
    if (c == CTRL_KEY('k')) {  /* Copy */
        if (E.sel_start_x != -1) {
            editor_copy_selection();
            if (E.mode == MODE_SELECTION) {
                editor_selection_clear();
                E.mode = MODE_NORMAL;
            }
            return;
        }
    } else if (c == CTRL_KEY('v')) {  /* Paste */
        editor_paste();
        return;
    } else if (c == CTRL_KEY('h')) {  /* Help */
        snprintf(E.statusmsg, sizeof(E.statusmsg),
            "HELP: cc=insert | Ctrl+Z=undo | Ctrl+Y=redo | Ctrl+A=select | Ctrl+K=copy");
        return;
    } else if (c == 8) {  /* Ctrl-Shift-H (often appears as ASCII BS, 8) */
        snprintf(E.statusmsg, sizeof(E.statusmsg),
            "ABC Vi v0.0.3 - A difficult terminal-based text editor");
        return;
    }
    
    /* Handle ESC key to exit modes */
    if (c == 27) {  /* ESC key */
        /* For better responsiveness, immediately process ESC without checking for sequence */
        if (E.mode != MODE_NORMAL) {
            /* Save previous mode to handle cursor position correctly */
            int prev_mode = E.mode;
            
            E.mode = MODE_NORMAL;
            /* Move cursor back only if coming from INSERT mode */
            if (prev_mode == MODE_INSERT && E.cx > 0 && E.numrows > 0)
                E.cx--;  /* Move cursor back by one */
            
            E.commandbuf[0] = '\0';
            E.commandlen = 0;
            editor_selection_clear();
            snprintf(E.statusmsg, sizeof(E.statusmsg), "-- NORMAL --");
            
            /* Clear any potential escape sequence that might be in the input buffer */
            nodelay(stdscr, TRUE);
            while (getch() != ERR);  /* Flush input buffer */
            nodelay(stdscr, FALSE);
        } else {
            /* If already in NORMAL mode, just clear any escape sequence */
            nodelay(stdscr, TRUE);
            while (getch() != ERR);  /* Flush input buffer */
            nodelay(stdscr, FALSE);
        }
        return;
    }
    /* Handle key based on current mode */
    switch (E.mode) {
        case MODE_NORMAL:
            /* Show NORMAL mode status */
            snprintf(E.statusmsg, sizeof(E.statusmsg), "-- NORMAL --");
            switch (c) {
/* ... */
                case 'c':  /* First 'c' of "cc" for insert mode (ABC Vi style) */
                    {
                        /* Check for second 'c' */
                        timeout(500);  /* Wait up to 500ms for second 'c' */
                        int next_c = getch();
                        timeout(100);  /* Reset timeout */
                        
                        if (next_c == 'c') {
                            E.mode = MODE_INSERT;
                            snprintf(E.statusmsg, sizeof(E.statusmsg), "-- INSERT --");
                        }
                        if (next_c != ERR) {
                            ungetch(next_c);  /* Put back character for next read */
                        }
                    }
                    break;
                case ':':
                    /* Enter command mode and reset command buffer */
                    E.mode = MODE_COMMAND;
                    E.commandbuf[0] = ':';
                    E.commandlen = 1;
                    E.commandbuf[E.commandlen] = '\0';
                    snprintf(E.statusmsg, sizeof(E.statusmsg), ":");
                    break;
                case 'x':  /* Delete character under cursor */
                    if (E.cx < E.row[E.cy].size) {
                        editor_insert_char(E.row[E.cy].chars[E.cx]);  /* For undo */
                        memmove(&E.row[E.cy].chars[E.cx], &E.row[E.cy].chars[E.cx + 1], E.row[E.cy].size - E.cx);
                        E.row[E.cy].size--;
                        E.dirty++;
                    }
                    break;
                case CTRL_KEY('a'):  /* Select all */
                    editor_select_all();
                    E.mode = MODE_SELECTION;
                    break;
                case CTRL_KEY('k'):  /* Copy */
                    if (E.sel_start_x != -1) {
                        editor_copy_selection();
                        editor_selection_clear();
                    } else {
                        snprintf(E.statusmsg, sizeof(E.statusmsg), "No selection to copy");
                    }
                    break;
                case CTRL_KEY('v'):  /* Paste */
                    editor_paste();
                    break;
                case CTRL_KEY('z'):  /* Undo */
                    editor_undo();
                    break;
                case CTRL_KEY('y'):  /* Redo */
                    editor_redo();
                    break;
                case 'v':  /* Visual (selection) mode */
                    E.mode = MODE_SELECTION;
                    editor_selection_start();
                    snprintf(E.statusmsg, sizeof(E.statusmsg), "-- VISUAL --");
                    break;
                case KEY_LEFT:
                case KEY_RIGHT:
                case KEY_UP:
                case KEY_DOWN:
                case 'h':
                case 'j':
                case 'k':
                case 'l':
                case KEY_HOME:
                case KEY_END:
                case KEY_PPAGE:
                case KEY_NPAGE:
                case '0':
                case '$':
                    editor_move_cursor(c);
                    break;
                case '\r':  /* Enter key */
                case KEY_ENTER: /* Some terminals send KEY_ENTER instead */
                    E.mode = MODE_INSERT;
                    snprintf(E.statusmsg, sizeof(E.statusmsg), "-- INSERT --");
                    break;
                /* Font size changes using Ctrl+Shift++ and Ctrl+Shift+- */
                case 43:  /* '+' key (may require different handling in some terminals) */
                    editor_change_font_size(1);
                    break;
                case 45:  /* '-' key */
                    editor_change_font_size(-1);
                    break;
                default:
                    break;
            }
            break;
            
        case MODE_INSERT:
            switch (c) {
                case 27:  /* ESC key - already handled above */
                    /* This should not be reached in normal cases */
                    break;

                /* Ctrl-C: Leave insert mode (like ESC) */
                case 3: /* Ctrl-C */
                    {
                        int prev_mode = E.mode;
                        E.mode = MODE_NORMAL;
                        /* Move cursor back only if coming from INSERT mode */
                        if (prev_mode == MODE_INSERT && E.cx > 0 && E.numrows > 0)
                            E.cx--;  /* Move cursor back by one */
                        E.commandbuf[0] = '\0';
                        E.commandlen = 0;
                        editor_selection_clear();
                        snprintf(E.statusmsg, sizeof(E.statusmsg), "-- NORMAL --");
                    }
                    break;
                /* No F1 key handling - use only ESC to exit insert mode */
                case KEY_BACKSPACE:
                case 127:  /* Also backspace on some terminals */
                    if (E.cx > 0 || E.cy > 0)
                        editor_del_char();
                    break;
                case KEY_LEFT:
                case KEY_RIGHT:
                case KEY_UP:
                case KEY_DOWN:
                case KEY_HOME:
                case KEY_END:
                case KEY_PPAGE:
                case KEY_NPAGE:
                    editor_move_cursor(c);
                    break;
                case CTRL_KEY('k'):  /* Copy (legacy, keep for compatibility) */
                    if (E.sel_start_x != -1) {
                        editor_copy_selection();
                        editor_selection_clear();
                    }
                    break;
                case CTRL_KEY('v'):  /* Paste */
                    editor_paste();
                    break;
                case CTRL_KEY('z'):  /* Undo */
                    editor_undo();
                    break;
                case CTRL_KEY('y'):  /* Redo */
                    editor_redo();
                    break;
                case '\r':  /* Enter key */
                case KEY_ENTER: /* Some terminals send KEY_ENTER instead */
                    editor_insert_newline();
                    break;
                default:
                    /* Accept all printable ASCII and Tab in insert mode */
                    if ((c >= 32 && c <= 126) || c == '\t') {
                        editor_insert_char(c);
                    }
                    break;
            }
            break;
            
        case MODE_COMMAND:
            switch (c) {
                case ':': /* Enter command mode */
                    E.mode = MODE_COMMAND;
                    memset(E.commandbuf, 0, sizeof(E.commandbuf));  /* Clear entire buffer */
                    E.commandbuf[0] = '\0';  /* Ensure null termination */
                    E.commandlen = 0;
                    break;
                case 27:  /* ESC key */
                    E.mode = MODE_NORMAL;
                    memset(E.commandbuf, 0, sizeof(E.commandbuf));  /* Clear entire buffer */
                    E.commandbuf[0] = '\0';  /* Ensure null termination */
                    E.commandlen = 0;
                    E.statusmsg[0] = '\0';
                    break;
                case '\r':  /* Enter key */
                case KEY_ENTER: /* Some terminals send KEY_ENTER instead */
                    if (E.commandlen > 0) {
                        /* Process command (includes validation and prefix handling) */
                        int should_quit = editor_process_command();
                        if (should_quit) return;  /* Quit if command requested */
                    }
                    /* Always return to normal mode after command */
                    E.mode = MODE_NORMAL;
                    memset(E.commandbuf, 0, sizeof(E.commandbuf));  /* Clear entire buffer */
                    E.commandbuf[0] = '\0';  /* Ensure null termination */
                    E.commandlen = 0;
                    break;
                case KEY_BACKSPACE:
                case 127:  /* Also backspace on some terminals */
                    if (E.commandlen > 0) {
                        E.commandlen--;
                        E.commandbuf[E.commandlen] = '\0';
                    }
                    break;
                default:
                    /* Add character to command buffer if printable ASCII (32-126) */
                    if (c >= 32 && c <= 126 && E.commandlen < (int)sizeof(E.commandbuf) - 1) {
                        /* Special handling for colon character */
                        if (c == ':') {
                            /* If this is the first character, add it normally */
                            if (E.commandlen == 0) {
                                E.commandbuf[E.commandlen++] = c;
                            }
                            /* Otherwise, don't add multiple colons at the start */
                            else if (E.commandbuf[0] == ':' && E.commandlen == 1) {
                                /* Skip adding another colon */
                            }
                            /* For other positions, add normally */
                            else {
                                E.commandbuf[E.commandlen++] = c;
                            }
                        } else {
                            /* Normal character - add it */
                            E.commandbuf[E.commandlen++] = c;
                        }
                        E.commandbuf[E.commandlen] = '\0';
                    }
                    break;
            }
            break;
            
        case MODE_SELECTION:
            switch (c) {
                case 27:  /* ESC key */
                    E.mode = MODE_NORMAL;
                    editor_selection_clear();
                    snprintf(E.statusmsg, sizeof(E.statusmsg), "-- NORMAL --");
                    break;
                case CTRL_KEY('k'):  /* Copy */
                    editor_copy_selection();
                    E.mode = MODE_NORMAL;
                    editor_selection_clear();
                    break;
                case 'y':  /* Yank (copy) */
                    editor_copy_selection();
                    E.mode = MODE_NORMAL;
                    editor_selection_clear();
                    break;
                case 'd':  /* Delete selection */
                    editor_copy_selection();  /* Copy first for undo capability */
                    
                    /* Normalize selection */
                    editor_selection_normalize();
                    
                    /* Handle single line case */
                    if (E.sel_start_y == E.sel_end_y) {
                        erow *row = &E.row[E.sel_start_y];
                        memmove(&row->chars[E.sel_start_x], &row->chars[E.sel_end_x], 
                                row->size - E.sel_end_x + 1);
                        row->size -= (E.sel_end_x - E.sel_start_x);
                        E.cx = E.sel_start_x;
                        E.cy = E.sel_start_y;
                    } else {
                        /* Handle multi-line case */
                        /* First line - keep start portion */
                        E.row[E.sel_start_y].size = E.sel_start_x;
                        E.row[E.sel_start_y].chars[E.sel_start_x] = '\0';
                        
                        /* Last line - keep end portion */
                        char *end_text = &E.row[E.sel_end_y].chars[E.sel_end_x];
                        int end_len = E.row[E.sel_end_y].size - E.sel_end_x;
                        
                        /* Add end part to first line */
                        erow *start_row = &E.row[E.sel_start_y];
                        char *new_buf = realloc(start_row->chars, start_row->size + end_len + 1);
                        if (new_buf == NULL) {
                            snprintf(E.statusmsg, sizeof(E.statusmsg), "Memory allocation failed");
                            return;
                        }
                        start_row->chars = new_buf;
                        memcpy(start_row->chars + start_row->size, end_text, end_len);
                        start_row->size += end_len;
                        start_row->chars[start_row->size] = '\0';
                        
                        /* Delete all rows in between */
                        for (int i = E.sel_end_y; i > E.sel_start_y; i--) {
                            editor_del_row(i);
                        }
                        
                        E.cx = E.sel_start_x;
                        E.cy = E.sel_start_y;
                    }
                    
                    E.dirty++;
                    E.mode = MODE_NORMAL;
                    editor_selection_clear();
                    break;
                case KEY_LEFT:
                case KEY_RIGHT:
                case KEY_UP:
                case KEY_DOWN:
                case 'h':
                case 'j':
                case 'k':
                case 'l':
                case KEY_HOME:
                case KEY_END:
                case KEY_PPAGE:
                case KEY_NPAGE:
                case '0':
                case '$':
                    editor_move_cursor(c);
                    editor_selection_update();
                    break;
                default:
                    break;
            }
            break;
    }
}

/* Free all memory and exit */
void editor_cleanup() {
    /* Free all memory */
    if (E.row) {
        for (int i = 0; i < E.numrows; i++) {
            if (E.row[i].chars) {
                free(E.row[i].chars);
                E.row[i].chars = NULL;  /* Prevent double-free issues */
            }
        }
        free(E.row);
        E.row = NULL; /* Prevent double-free issues */
    }
    
    /* Free clipboard */
    if (E.clipboard) {
        for (int i = 0; i < E.clipboard_len; i++) {
            if (E.clipboard[i]) {
                free(E.clipboard[i]);
                E.clipboard[i] = NULL; /* Prevent double-free issues */
            }
        }
        free(E.clipboard);
        E.clipboard = NULL;
        E.clipboard_len = 0;
    }
    
    /* Free undo/redo stacks */
    free_operations_stack(E.undo_stack);
    E.undo_stack = NULL;
    
    free_operations_stack(E.redo_stack);
    E.redo_stack = NULL;
    
    /* Free filename */
    if (E.filename) {
        free(E.filename);
        E.filename = NULL;
    }
    
    /* Clear screen and reset terminal */
    clear();
    refresh();
    /* Reset terminal attributes and close ncurses */
    reset_shell_mode();
    endwin();
}

/* Main function */
int main(int argc, char *argv[]) {
    /* Initialize ncurses */
    if (initscr() == NULL) {
        fprintf(stderr, "Error initializing ncurses\n");
        return 1;
    }
    
    /* Use the full screen */
    resize_term(0, 0); /* Request maximum terminal size */
#ifndef _WIN32
    signal(SIGINT, SIG_IGN); /* Ignore Ctrl-C (SIGINT) so we can handle it as a key */
#endif
    
    /* Terminal setup */
    raw();              /* Raw mode */
    keypad(stdscr, TRUE);  /* Enable keypad */
    noecho();           /* Don't echo input */
    timeout(100);       /* Non-blocking input with 100ms timeout */
    
    /* Set terminal to raw mode */
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(editor_cleanup);
    
    /* Enable raw mode */
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
    
    /* Initialize the editor */
    init_editor();
    
    /* Process command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            /* Clean up ncurses properly */
            clear();
            refresh();
            reset_shell_mode();
            endwin();
            printf("Usage: %s [options] [file]\n", argv[0]);
            printf("Options:\n");
            printf("  -h, --help     Show this help message\n");
            printf("  -v, --version  Show version information\n");
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            /* Clean up ncurses properly */
            clear();
            refresh();
            reset_shell_mode();
            endwin();
            printf("ABC Vi version 0.0.3\n");
            return 0;
        } else {
            /* Treat as filename */
            editor_open(argv[i]);
        }
    }
    
    /* Set initial status message */
    snprintf(E.statusmsg, sizeof(E.statusmsg), 
             "HELP: Press Ctrl+H for help | cc for insert mode | Ctrl+Shift+Q to quit");
    
    /* Set initial mode to NORMAL */
    E.mode = MODE_NORMAL;
    
    /* Main loop with error handling */
    while (1) {
        /* Clear any previous errors */
        errno = 0;
        
        /* Update screen */
        editor_refresh_screen();
        
        /* Check for system errors */
        if (errno != 0) {
            snprintf(E.statusmsg, sizeof(E.statusmsg), 
                     "Error: %s", strerror(errno));
        }
        
        /* Process user input */
        editor_process_keypress();
        
        /* Handle terminal resize */
        #ifdef SIGWINCH
            struct winsize w;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
                if (w.ws_row != E.screenrows + 2 || w.ws_col != E.screencols) {
                    /* Terminal size changed */
                    endwin();
                    refresh();
                    clear();
                    getmaxyx(stdscr, E.screenrows, E.screencols);
                    if (E.screenrows < 3) E.screenrows = 3;  /* Minimum size */
                    if (E.screencols < 20) E.screencols = 20;  /* Minimum width */
                    E.screenrows -= 2;  /* Adjust for status and command lines */
                    
                    /* Force a complete redraw */
                    editor_refresh_screen();
                    
                    /* Ensure cursor is within visible area */
                    if (E.cx > E.screencols - 1) E.cx = E.screencols - 1;
                    if (E.cy > E.screenrows - 1) E.cy = E.screenrows - 1;
                }
            }
        #endif
    }
    
    return 0;
}
