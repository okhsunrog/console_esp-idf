/* linenoise.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 *
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2023-2023, Danila Gornushko <d3g3v3 at gmail dot com>
 * Copyright (c) 2010-2023, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ------------------------------------------------------------------------
 *
 * References:
 * -http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * When linenoiseClearScreen() is called, two additional escape sequences
 * are used in order to clear the screen and position the cursor at home
 * position.
 *
 * CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 */

// ReSharper disable CppDefaultCaseNotHandledInSwitchStatement
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include "linenoise.h"

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_DEFAULT_MAX_LINE 4096
#define LINENOISE_MINIMAL_MAX_LINE 64
#define LINENOISE_COMMAND_MAX_LEN 32
#define LINENOISE_PASTE_KEY_DELAY 30 /* Delay, in milliseconds, between two characters being pasted from clipboard */

static linenoiseCompletionCallback *completionCallback = NULL;
static linenoiseHintsCallback *hintsCallback = NULL;
static linenoiseFreeHintsCallback *freeHintsCallback = NULL;
static void refreshLineWithCompletion(struct linenoiseState *ls, const linenoiseCompletions *lc, int flags);
static void refreshLineWithFlags(struct linenoiseState *l, int flags);

static int maskmode = 0; /* Show "***" instead of input. For passwords. */
static size_t max_cmdline_length = LINENOISE_DEFAULT_MAX_LINE;
static int mlmode = 0;  /* Multi line mode. Default is single line. */
static int dumbmode = 0; /* Dumb mode where line editing is disabled. Off by default */
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
static char **history = NULL;

SemaphoreHandle_t stdout_taken_sem;


enum KEY_ACTION{
	KEY_NULL = 0,	    /* NULL */
	CTRL_A = 1,         /* Ctrl+a */
	CTRL_B = 2,         /* Ctrl-b */
	CTRL_C = 3,         /* Ctrl-c */
	CTRL_D = 4,         /* Ctrl-d */
	CTRL_E = 5,         /* Ctrl-e */
	CTRL_F = 6,         /* Ctrl-f */
	CTRL_H = 8,         /* Ctrl-h */
	TAB = 9,            /* Tab */
	CTRL_K = 11,        /* Ctrl+k */
	CTRL_L = 12,        /* Ctrl+l */
	ENTER = 10,         /* Enter */
	CTRL_N = 14,        /* Ctrl-n */
	CTRL_P = 16,        /* Ctrl-p */
	CTRL_T = 20,        /* Ctrl-t */
	CTRL_U = 21,        /* Ctrl+u */
	CTRL_W = 23,        /* Ctrl+w */
	ESC = 27,           /* Escape */
	BACKSPACE =  127    /* Backspace */
};

int linenoiseHistoryAdd(const char *line);
#define REFRESH_CLEAN (1<<0)    // Clean the old prompt from the screen
#define REFRESH_WRITE (1<<1)    // Rewrite the prompt on the screen.
#define REFRESH_ALL (REFRESH_CLEAN|REFRESH_WRITE) // Do both.
static void refreshLine(struct linenoiseState *l);

/* ======================= Low level terminal handling ====================== */

/* Enable "mask mode". When it is enabled, instead of the input that
 * the user is typing, the terminal will just display a corresponding
 * number of asterisks, like "****". This is useful for passwords and other
 * secrets that should not be displayed. */
void linenoiseMaskModeEnable(void) {
    maskmode = 1;
}

/* Disable mask mode. */
void linenoiseMaskModeDisable(void) {
    maskmode = 0;
}

/* Set if to use or not the multi line mode. */
void linenoiseSetMultiLine(int ml) {
    mlmode = ml;
}

/* Set if terminal does not recognize escape sequences */
void linenoiseSetDumbMode(int set) {
    dumbmode = set;
}

/* Returns whether the current mode is dumbmode or not. */
bool linenoiseIsDumbMode(void) {
    return dumbmode;
}

void flushWrite(void) {
    if (__fbufsize(stdout) > 0) {
        fflush(stdout);
    }
    fsync(fileno(stdout));
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor. */
static int getCursorPosition(void) {
    char buf[LINENOISE_COMMAND_MAX_LEN] = { 0 };
    int cols = 0;
    int rows = 0;
    int i = 0;
    const int out_fd = fileno(stdout);
    const int in_fd = fileno(stdin);
    /* The following ANSI escape sequence is used to get from the TTY the
     * cursor position. */
    const char get_cursor_cmd[] = "\x1b[6n";

    /* Send the command to the TTY on the other end of the UART.
     * Let's use unistd's write function. Thus, data sent through it are raw
     * reducing the overhead compared to using fputs, fprintf, etc... */
    write(out_fd, get_cursor_cmd, sizeof(get_cursor_cmd));

    /* For USB CDC, it is required to flush the output. */
    flushWrite();

    /* The other end will send its response which format is ESC [ rows ; cols R
     * We don't know exactly how many bytes we have to read, thus, perform a
     * read for each byte.
     * Stop right before the last character of the buffer, to be able to NULL
     * terminate it. */
    while (i < sizeof(buf)-1) {
        /* Keep using unistd's functions. Here, using `read` instead of `fgets`
         * or `fgets` guarantees us that we we can read a byte regardless on
         * whether the sender sent end of line character(s) (CR, CRLF, LF). */
        if (read(in_fd, buf + i, 1) != 1 || buf[i] == 'R') {
            /* If we couldn't read a byte from STDIN or if 'R' was received,
             * the transmission is finished. */
            break;
        }

        /* For some reasons, it is possible that we receive new line character
         * after querying the cursor position on some UART. Let's ignore them,
         * this will not affect the rest of the program. */
        if (buf[i] != '\n') {
            i++;
        }
    }

    /* NULL-terminate the buffer, this is required by `sscanf`. */
    buf[i] = '\0';

    /* Parse the received data to get the position of the cursor. */
    if (buf[0] != ESC || buf[1] != '[' || sscanf(buf+2,"%d;%d",&rows,&cols) != 2) {
        return -1;
    }
    return cols;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int getColumns(void) {
    char seq[LINENOISE_COMMAND_MAX_LEN] = { 0 };
    const int fd = fileno(stdout);

    /* The following ANSI escape sequence is used to tell the TTY to move
     * the cursor to the most-right position. */
    const char move_cursor_right[] = "\x1b[999C";
    const size_t cmd_len = sizeof(move_cursor_right);

    /* This one is used to set the cursor position. */
    const char set_cursor_pos[] = "\x1b[%dD";

    /* Get the initial position so we can restore it later. */
    const int start = getCursorPosition();
    if (start == -1) {
        goto failed;
    }

    /* Send the command to go to right margin. Use `write` function instead of
     * `fwrite` for the same reasons explained in `getCursorPosition()` */
    if (write(fd, move_cursor_right, cmd_len) != cmd_len) {
        goto failed;
    }
    flushWrite();

    /* After sending this command, we can get the new position of the cursor,
     * we'd get the size, in columns, of the opened TTY. */
    const int cols = getCursorPosition();
    if (cols == -1) {
        goto failed;
    }

    /* Restore the position of the cursor back. */
    if (cols > start) {
        /* Generate the move cursor command. */
        const int written = snprintf(seq, LINENOISE_COMMAND_MAX_LEN, set_cursor_pos, cols - start);

        /* If `written` is equal or bigger than LINENOISE_COMMAND_MAX_LEN, it
         * means that the output has been truncated because the size provided
         * is too small. */
        assert (written < LINENOISE_COMMAND_MAX_LEN);

        /* Send the command with `write`, which is not buffered. */
        if (write(fd, seq, written) == -1) {
            /* Can't recover... */
        }
        flushWrite();
    }
    return cols;

failed:
    return 80;
}

/* Clear the screen. Used to handle ctrl+l */
void linenoiseClearScreen(void) {
    fprintf(stdout,"\x1b[H\x1b[2J");
    flushWrite();
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void linenoiseBeep(void) {
    fprintf(stdout, "\x7");
    flushWrite();
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static void freeCompletions(const linenoiseCompletions *lc) {
    for (size_t i = 0; i < lc->len; i++)
        // ReSharper disable once CppDFANullDereference
        free(lc->cvec[i]);
}

/* Called by completeLine() and linenoiseShow() to render the current
 * edited line with the proposed completion. If the current completion table
 * is already available, it is passed as second argument, otherwise the
 * function will use the callback to obtain it.
 *
 * Flags are the same as refreshLine*(), that is REFRESH_* macros. */
static void refreshLineWithCompletion(struct linenoiseState *ls, const linenoiseCompletions *lc, int flags) {
    /* Obtain the table of completions if the caller didn't provide one. */
    linenoiseCompletions ctable = { 0, NULL };
    if (lc == NULL) {
        completionCallback(ls->buf,&ctable);
        lc = &ctable;
    }

    /* Show the edited line with completion if possible, or just refresh. */
    if (ls->completion_idx < lc->len) {
        const struct linenoiseState saved = *ls;
        ls->len = ls->pos = strlen(lc->cvec[ls->completion_idx]);
        ls->buf = lc->cvec[ls->completion_idx];
        refreshLineWithFlags(ls,flags);
        ls->len = saved.len;
        ls->pos = saved.pos;
        ls->buf = saved.buf;
    } else {
        refreshLineWithFlags(ls,flags);
    }

    /* Free the completions table if needed. */
    if (lc != &ctable) freeCompletions(&ctable);
}

/* This is an helper function for linenoiseEdit*() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition.
 *
 * If the function returns non-zero, the caller should handle the
 * returned value as a byte read from the standard input, and process
 * it as usually: this basically means that the function may return a byte
 * read from the termianl but not processed. Otherwise, if zero is returned,
 * the input was consumed by the completeLine() function to navigate the
 * possible completions, and the caller should read for the next characters
 * from stdin. */
static int completeLine(struct linenoiseState *ls, int keypressed) {
    linenoiseCompletions lc = { 0, NULL };
    char c = keypressed;

    completionCallback(ls->buf,&lc);
    if (lc.len == 0) {
        linenoiseBeep();
        ls->in_completion = 0;
    } else {
   
        switch(c) {
            case 9: /* tab */
                if (ls->in_completion == 0) {
                    ls->in_completion = 1;
                    ls->completion_idx = 0;
                } else {
                    ls->completion_idx = (ls->completion_idx+1) % (lc.len+1);
                    if (ls->completion_idx == lc.len) linenoiseBeep();
                }
                c = 0;
                break;
            case 27: /* escape */
                /* Re-show original buffer */
                if (ls->completion_idx < lc.len) refreshLine(ls);
                ls->in_completion = 0;
                c = 0;
                break;
            default:
                /* Update buffer and return */
                if (ls->completion_idx < lc.len) {
                    const int nwritten = snprintf(ls->buf, ls->buflen, "%s", lc.cvec[ls->completion_idx]);
                    ls->len = ls->pos = nwritten;
                }
                ls->in_completion = 0;
                break;
        }

        /* Show completion or original buffer */
        if (ls->in_completion && ls->completion_idx < lc.len) {
            refreshLineWithCompletion(ls,&lc,REFRESH_ALL);
        } else {
            refreshLine(ls);
        }
    }

    freeCompletions(&lc);
    return c; /* Return last read character */
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) {
    completionCallback = fn;
}

/* Register a hits function to be called to show hits to the user at the
 * right of the prompt. */
void linenoiseSetHintsCallback(linenoiseHintsCallback *fn) {
    hintsCallback = fn;
}

/* Register a function to free the hints returned by the hints callback
 * registered with linenoiseSetHintsCallback(). */
void linenoiseSetFreeHintsCallback(linenoiseFreeHintsCallback *fn) {
    freeHintsCallback = fn;
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str) {
    const size_t len = strlen(str);

    char* copy = malloc(len + 1);
    if (copy == NULL) return;
    memcpy(copy,str,len+1);
    char** cvec = realloc(lc->cvec, sizeof(char*) * (lc->len + 1));
    if (cvec == NULL) {
        free(copy);
        return;
    }
    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
};

static void abInit(struct abuf *ab) {
    ab->b = NULL;
    ab->len = 0;
}

static void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b,ab->len+len);

    if (new == NULL) return;
    memcpy(new+ab->len,s,len);
    ab->b = new;
    ab->len += len;
}

static void abFree(const struct abuf *ab) {
    free(ab->b);
}

/* Helper of refreshSingleLine() and refreshMultiLine() to show hints
 * to the right of the prompt. */
void refreshShowHints(struct abuf *ab, const struct linenoiseState *l, int plen) {
    if (hintsCallback && plen+l->len < l->cols) {
        int color = -1, bold = 0;
        char *hint = hintsCallback(l->buf,&color,&bold);
        if (hint) {
            char seq[64];
            int hintlen = strlen(hint);
            const int hintmaxlen = l->cols-(plen+l->len);
            if (hintlen > hintmaxlen) hintlen = hintmaxlen;
            if (bold == 1 && color == -1) color = 37;
            if (color != -1 || bold != 0)
                snprintf(seq,64,"\033[%d;%dm",bold,color);
            else
                seq[0] = '\0';
            abAppend(ab,seq,strlen(seq));
            abAppend(ab,hint,hintlen);
            if (color != -1 || bold != 0)
                abAppend(ab,"\033[0m",4);
            /* Call the function to free the hint returned. */
            if (freeHintsCallback) freeHintsCallback(hint);
        }
    }
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal.
 *
 * Flags is REFRESH_* macros. The function can just remove the old
 * prompt, just write it, or both. */
static void refreshSingleLine(const struct linenoiseState *l, int flags) {
    char seq[64];
    const size_t plen = l->plen;
    const char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    struct abuf ab;

    while((plen+pos) >= l->cols) {
        buf++;
        len--;
        pos--;
    }
    while (plen+len > l->cols) {
        len--;
    }

    abInit(&ab);
    /* Cursor to left edge */
    snprintf(seq,sizeof(seq),"\r");
    abAppend(&ab,seq,strlen(seq));

    if (flags & REFRESH_WRITE) {
        /* Write the prompt and the current buffer content */
        abAppend(&ab,l->prompt,strlen(l->prompt));
        if (maskmode == 1) {
            while (len--) abAppend(&ab,"*",1);
        } else {
            abAppend(&ab,buf,len);
        }
        /* Show hits if any. */
        refreshShowHints(&ab,l,plen);
    }
    /* Erase to right */
    snprintf(seq,sizeof(seq),"\x1b[0K");
    abAppend(&ab,seq,strlen(seq));

    if (flags & REFRESH_WRITE) {
        /* Move cursor to original position. */
        snprintf(seq,sizeof(seq),"\r\x1b[%dC", (int)(pos+plen));
        abAppend(&ab,seq,strlen(seq));
    }

    if (fwrite(ab.b, ab.len, 1, stdout) == -1) {} /* Can't recover from write error. */
    flushWrite();
    abFree(&ab);
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal.
 *
 * Flags is REFRESH_* macros. The function can just remove the old
 * prompt, just write it, or both. */
static void refreshMultiLine(struct linenoiseState *l, int flags) {
    char seq[64];
    const int plen = l->plen;
    int rows = (plen+l->len+l->cols-1)/l->cols; /* rows used by current buf. */
    const int rpos = (plen+l->oldpos+l->cols)/l->cols; /* cursor relative row. */
    const int old_rows = l->oldrows;
    struct abuf ab;

    l->oldrows = rows;

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    abInit(&ab);

    if (flags & REFRESH_CLEAN) {
        if (old_rows-rpos > 0) {
            snprintf(seq,64,"\x1b[%dB", old_rows-rpos);
            abAppend(&ab,seq,strlen(seq));
        }

        /* Now for every row clear it, go up. */
        for (int j = 0; j < old_rows-1; j++) {
            snprintf(seq,64,"\r\x1b[0K\x1b[1A");
            abAppend(&ab,seq,strlen(seq));
        }
    }

    if (flags & REFRESH_ALL) {
        /* Clean the top line. */
        snprintf(seq,64,"\r\x1b[0K");
        abAppend(&ab,seq,strlen(seq));
    }

    if (flags & REFRESH_WRITE) {
        /* Write the prompt and the current buffer content */
        abAppend(&ab,l->prompt,strlen(l->prompt));
        if (maskmode == 1) {
            for (unsigned int i = 0; i < l->len; i++) abAppend(&ab,"*",1);
        } else {
            abAppend(&ab,l->buf,l->len);
        }

        /* Show hits if any. */
        refreshShowHints(&ab,l,plen);

        /* If we are at the very end of the screen with our prompt, we need to
         * emit a newline and move the prompt to the first column. */
        if (l->pos &&
            l->pos == l->len &&
            (l->pos+plen) % l->cols == 0)
        {
            abAppend(&ab,"\n",1);
            snprintf(seq,64,"\r");
            abAppend(&ab,seq,strlen(seq));
            rows++;
            if (rows > (int)l->oldrows) l->oldrows = rows;
        }

        /* Move cursor to right position. */
        const int rpos2 = (plen + l->pos + l->cols) / l->cols; /* Current cursor relative row */

        /* Go up till we reach the expected positon. */
        if (rows-rpos2 > 0) {
            snprintf(seq,64,"\x1b[%dA", rows-rpos2);
            abAppend(&ab,seq,strlen(seq));
        }

        /* Set column. */
        const int col = (plen + (int) l->pos) % (int) l->cols;
        if (col)
            snprintf(seq,64,"\r\x1b[%dC", col);
        else
            snprintf(seq,64,"\r");
        abAppend(&ab,seq,strlen(seq));
    }

    l->oldpos = l->pos;

    if (fwrite(ab.b, ab.len, 1, stdout) == -1) {} /* Can't recover from write error. */
    flushWrite();
    abFree(&ab);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void refreshLineWithFlags(struct linenoiseState *l, int flags) {
    if (mlmode)
        refreshMultiLine(l,flags);
    else
        refreshSingleLine(l,flags);
}

/* Utility function to avoid specifying REFRESH_ALL all the times. */
static void refreshLine(struct linenoiseState *l) {
    refreshLineWithFlags(l,REFRESH_ALL);
}

/* Hide the current line, when using the multiplexing API. */
void linenoiseHide(struct linenoiseState *l) {
    if (mlmode)
        refreshMultiLine(l,REFRESH_CLEAN);
    else
        refreshSingleLine(l,REFRESH_CLEAN);
}

/* Show the current line, when using the multiplexing API. */
void linenoiseShow(struct linenoiseState *l) {
    if (l->in_completion) {
        refreshLineWithCompletion(l,NULL,REFRESH_WRITE);
    } else {
        refreshLineWithFlags(l,REFRESH_WRITE);
    }
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
int linenoiseEditInsert(struct linenoiseState *l, char c) {
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            l->buf[l->pos] = c;
            l->pos++;
            l->len++;
            l->buf[l->len] = '\0';
            if ((!mlmode && l->plen+l->len < l->cols && !hintsCallback)) {
                /* Avoid a full update of the line in the
                 * trivial case. */
                const char d = (maskmode==1) ? '*' : c;
                if (fwrite(&d,1,1,stdout) == -1) return -1;
                flushWrite();
            } else {
                refreshLine(l);
            }
        } else {
            memmove(l->buf+l->pos+1,l->buf+l->pos,l->len-l->pos);
            l->buf[l->pos] = c;
            l->len++;
            l->pos++;
            l->buf[l->len] = '\0';
            refreshLine(l);
        }
    }
    return 0;
}

int linenoiseInsertPastedChar(struct linenoiseState *l, char c) {
    const int fd = fileno(stdout);
    if (l->len < l->buflen && l->len == l->pos) {
        l->buf[l->pos] = c;
        l->pos++;
        l->len++;
        l->buf[l->len] = '\0';
        if (write(fd, &c,1) == -1) {
            return -1;
        }
        flushWrite();
    }
    return 0;
}

/* Move cursor on the left. */
void linenoiseEditMoveLeft(struct linenoiseState *l) {
    if (l->pos > 0) {
        l->pos--;
        refreshLine(l);
    }
}

/* Move cursor on the right. */
void linenoiseEditMoveRight(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos++;
        refreshLine(l);
    }
}

/* Move cursor to the start of the line. */
void linenoiseEditMoveHome(struct linenoiseState *l) {
    if (l->pos != 0) {
        l->pos = 0;
        refreshLine(l);
    }
}

/* Move cursor to the end of the line. */
void linenoiseEditMoveEnd(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos = l->len;
        refreshLine(l);
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
void linenoiseEditHistoryNext(struct linenoiseState *l, int dir) {
    if (history_len > 1) {
        /* Update the current history entry before to
         * overwrite it with the next one. */
        free(history[history_len - 1 - l->history_index]);
        history[history_len - 1 - l->history_index] = strdup(l->buf);
        /* Show the new entry */
        l->history_index += (dir == LINENOISE_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        } else if (l->history_index >= history_len) {
            l->history_index = history_len-1;
            return;
        }
        strncpy(l->buf,history[history_len - 1 - l->history_index],l->buflen);
        l->buf[l->buflen-1] = '\0';
        l->len = l->pos = strlen(l->buf);
        refreshLine(l);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
void linenoiseEditDelete(struct linenoiseState *l) {
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf+l->pos,l->buf+l->pos+1,l->len-l->pos-1);
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Backspace implementation. */
void linenoiseEditBackspace(struct linenoiseState *l) {
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf+l->pos-1,l->buf+l->pos,l->len-l->pos);
        l->pos--;
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Delete the previosu word, maintaining the cursor at the start of the
 * current word. */
void linenoiseEditDeletePrevWord(struct linenoiseState *l) {
    const size_t old_pos = l->pos;

    while (l->pos > 0 && l->buf[l->pos-1] == ' ')
        l->pos--;
    while (l->pos > 0 && l->buf[l->pos - 1] != ' ')
        l->pos--;
    const size_t diff = old_pos - l->pos;
    memmove(l->buf+l->pos,l->buf+old_pos,l->len-old_pos+1);
    l->len -= diff;
    refreshLine(l);
}

// TODO: try to make a non-blocking dumb mode
static char *linenoiseDumb(struct linenoiseState *l) {
    /* dumb terminal, fall back to fgets */
    // Not needed anymore, prompt is now in linenoiseEditStart
    // fputs(l->prompt, stdout);
    // flushWrite();
    l->len = 0; //needed?
    while (l->len < l->buflen) {
        const int c = fgetc(stdin);
        xSemaphoreTake(stdout_taken_sem, portMAX_DELAY);
        if (c == '\n') {
            xSemaphoreGive(stdout_taken_sem);
            break;
        } else if (c >= 0x1c && c <= 0x1f){
            xSemaphoreGive(stdout_taken_sem);
            continue; /* consume arrow keys */
        } else if (c == BACKSPACE || c == 0x8) {
            if (l->len > 0) {
                l->buf[l->len - 1] = 0;
                l->len --;
            }
            fputs("\x08 ", stdout); /* Windows CMD: erase symbol under cursor */
            flushWrite();
        } else {
            l->buf[l->len] = c;
            l->len++;
        }
        fputc(c, stdout); /* echo */
        flushWrite();
        xSemaphoreGive(stdout_taken_sem);
    }
    xSemaphoreTake(stdout_taken_sem, portMAX_DELAY);
    fputc('\n', stdout);
    flushWrite();
    xSemaphoreGive(stdout_taken_sem);
    // if (l->len == 0) return linenoiseEditMore;
    return strdup(l->buf);
}

uint32_t getMillis(void) {
    struct timeval tv = { 0 };
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* This function is part of the multiplexed API of Linenoise, that is used
 * in order to implement the blocking variant of the API but can also be
 * called by the user directly in an event driven program. It will:
 *
 * 1. Initialize the linenoise state passed by the user.
 * 2. Put the terminal in RAW mode.
 * 3. Show the prompt.
 * 4. Return control to the user, that will have to call linenoiseEditFeed()
 *    each time there is some data arriving in the standard input.
 *
 * The user can also call linenoiseEditHide() and linenoiseEditShow() if it
 * is required to show some input arriving asyncronously, without mixing
 * it with the currently edited line.
 *
 * When linenoiseEditFeed() returns non-NULL, the user finished with the
 * line editing session (pressed enter CTRL-D/C): in this case the caller
 * needs to call linenoiseEditStop() to put back the terminal in normal
 * mode. This will not destroy the buffer, as long as the linenoiseState
 * is still valid in the context of the caller.
 *
 * The function returns 0 on success, or -1 if writing to standard output
 * fails. If stdin_fd or stdout_fd are set to -1, the default is to use
 * STDIN_FILENO and STDOUT_FILENO.
 */
int linenoiseEditStart(struct linenoiseState *l) {
    /* Populate the linenoise state that we pass to functions implementing
     * specific editing functionalities. */
    l->in_completion = 0;
    // l->plen = strlen(l->prompt);
    l->oldpos = l->pos = 0;
    l->len = 0;
    l->cols = getColumns();
    l->oldrows = 0;
    l->history_index = 0;

    /* Buffer starts empty. */
    l->buf[0] = '\0';
    l->buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    xSemaphoreTake(stdout_taken_sem, portMAX_DELAY);
    if (!dumbmode) {
        linenoiseHistoryAdd("");
        // int pos1 = getCursorPosition();
        if (fwrite(l->prompt,l->plen,1,stdout) == -1) {
            xSemaphoreGive(stdout_taken_sem);
            return -1;
        }
        flushWrite();
        // int pos2 = getCursorPosition();
        // if (pos1 >= 0 && pos2 >= 0) {
        //     l->plen = pos2 - pos1;
        // }
    } else {
        if (fwrite(l->prompt,l->plen,1,stdout) == -1) {
            xSemaphoreGive(stdout_taken_sem);
            return -1;
        }
        flushWrite();
    }
    xSemaphoreGive(stdout_taken_sem);
    return 0;
}

char *linenoiseEditMore = "If you see this, you are misusing the API: when linenoiseEditFeed() is called, if it returns linenoiseEditMore the user is yet editing the line. See the README file for more information.";

/* This function is part of the multiplexed API of linenoise, see the top
 * comment on linenoiseEditStart() for more information. Call this function
 * each time there is some data to read from the standard input file
 * descriptor. In the case of blocking operations, this function can just be
 * called in a loop, and block.
 *
 * The function returns linenoiseEditMore to signal that line editing is still
 * in progress, that is, the user didn't yet pressed enter / CTRL-D. Otherwise
 * the function returns the pointer to the heap-allocated buffer with the
 * edited line, that the user should free with linenoiseFree().
 *
 * On special conditions, NULL is returned and errno is populated:
 *
 * EAGAIN if the user pressed Ctrl-C
 * ENOENT if the user pressed Ctrl-D
 *
 * Some other errno: I/O error.
 */
char *linenoiseEditFeed(struct linenoiseState *l) {
    if (dumbmode) return linenoiseDumb(l);
    char c;
    char seq[3];

    /*
     * To determine whether the user is pasting data or typing itself, we
     * need to calculate how many milliseconds elapsed between two key
     * presses. Indeed, if there is less than LINENOISE_PASTE_KEY_DELAY
     * (typically 30-40ms), then a paste is being performed, else, the
     * user is typing.
     * NOTE: pressing a key down without releasing it will also spend
     * about 40ms (or even more)
     */
    const uint32_t t1 = getMillis();
    const int nread = fread(&c, 1, 1, stdin);
    if (nread <= 0) return linenoiseEditMore;
    const uint32_t t2 = getMillis();
    xSemaphoreTake(stdout_taken_sem, portMAX_DELAY);
    // FIXME: line printed twice after pasting something that takes more than 1 line
    if ( (t2 - t1) < LINENOISE_PASTE_KEY_DELAY && c != ENTER) {
        /* Pasting data, insert characters without formatting.
         * This can only be performed when the cursor is at the end of the
         * line. */
        if (linenoiseInsertPastedChar(l,c)) {
            errno = EIO;
            xSemaphoreGive(stdout_taken_sem);
            return NULL;
        }
        xSemaphoreGive(stdout_taken_sem);
        return linenoiseEditMore;
    }

    /* Only autocomplete when the callback is set. It returns < 0 when
     * there was an error reading from fd. Otherwise it will return the
     * character that should be handled next. */
        if ((l->in_completion || c == 9) && completionCallback != NULL) {
        c = completeLine(l,c);
        /* Return on errors */
        // TODO: how was it supposed to work? c can't be less than 0
        // if (c < 0) return NULL;
        /* Read next character when 0 */
        if (c == 0) {
            xSemaphoreGive(stdout_taken_sem);
            return linenoiseEditMore;
        }
    }

    switch(c) {
    case ENTER:    /* enter */
        history_len--;
        free(history[history_len]);
        if (mlmode) linenoiseEditMoveEnd(l);
        if (hintsCallback) {
            /* Force a refresh without hints to leave the previous
             * line as the user typed it after a newline. */
            linenoiseHintsCallback *hc = hintsCallback;
            hintsCallback = NULL;
            refreshLine(l);
            hintsCallback = hc;
        }
        xSemaphoreGive(stdout_taken_sem);
        return strdup(l->buf);
    case CTRL_C:     /* ctrl-c */
        errno = EAGAIN;
        xSemaphoreGive(stdout_taken_sem);
        return NULL;
    case BACKSPACE:   /* backspace */
    case 8:     /* ctrl-h */
        linenoiseEditBackspace(l);
        break;
    case CTRL_D:     /* ctrl-d, remove char at right of cursor, or if the
                        line is empty, act as end-of-file. */
        if (l->len > 0) {
            linenoiseEditDelete(l);
        } else {
            history_len--;
            free(history[history_len]);
            errno = ENOENT;
            xSemaphoreGive(stdout_taken_sem);
            return NULL;
        }
        break;
    case CTRL_T:    /* ctrl-t, swaps current character with previous. */
        if (l->pos > 0 && l->pos < l->len) {
            const int aux = l->buf[l->pos-1];
            l->buf[l->pos-1] = l->buf[l->pos];
            l->buf[l->pos] = aux;
            if (l->pos != l->len-1) l->pos++;
            refreshLine(l);
        }
        break;
    case CTRL_B:     /* ctrl-b */
        linenoiseEditMoveLeft(l);
        break;
    case CTRL_F:     /* ctrl-f */
        linenoiseEditMoveRight(l);
        break;
    case CTRL_P:    /* ctrl-p */
        linenoiseEditHistoryNext(l, LINENOISE_HISTORY_PREV);
        break;
    case CTRL_N:    /* ctrl-n */
        linenoiseEditHistoryNext(l, LINENOISE_HISTORY_NEXT);
        break;
    case ESC:    /* escape sequence */
        /* Read the next two bytes representing the escape sequence.
         * chars at different times. */
        if (fread(seq, 1, 1, stdin) == -1) break;
        if (fread(seq+1, 1, 1, stdin) == -1) break;
        // or just if (fread(seq, 1, 2, stdin) < 2) break;

        /* ESC [ sequences. */
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                /* Extended escape, read additional byte. */
                if (fread(seq+2, 1, 1, stdin) == -1) break;
                if (seq[2] == '~') {
                    switch(seq[1]) {
                    case '3': /* Delete key. */
                        linenoiseEditDelete(l);
                        break;
                    }
                }
            } else {
                switch(seq[1]) {
                case 'A': /* Up */
                    linenoiseEditHistoryNext(l, LINENOISE_HISTORY_PREV);
                    break;
                case 'B': /* Down */
                    linenoiseEditHistoryNext(l, LINENOISE_HISTORY_NEXT);
                    break;
                case 'C': /* Right */
                    linenoiseEditMoveRight(l);
                    break;
                case 'D': /* Left */
                    linenoiseEditMoveLeft(l);
                    break;
                case 'H': /* Home */
                    linenoiseEditMoveHome(l);
                    break;
                case 'F': /* End*/
                    linenoiseEditMoveEnd(l);
                    break;
                }
            }
        }

        /* ESC O sequences. */
        else if (seq[0] == 'O') {
            switch(seq[1]) {
            case 'H': /* Home */
                linenoiseEditMoveHome(l);
                break;
            case 'F': /* End*/
                linenoiseEditMoveEnd(l);
                break;
            }
        }
        break;
    default:
        if (linenoiseEditInsert(l,c)) {
            xSemaphoreGive(stdout_taken_sem);
            return NULL;
        }
        break;
    case CTRL_U: /* Ctrl+u, delete the whole line. */
        l->buf[0] = '\0';
        l->pos = l->len = 0;
        refreshLine(l);
        break;
    case CTRL_K: /* Ctrl+k, delete from current to end of line. */
        l->buf[l->pos] = '\0';
        l->len = l->pos;
        refreshLine(l);
        break;
    case CTRL_A: /* Ctrl+a, go to the start of the line */
        linenoiseEditMoveHome(l);
        break;
    case CTRL_E: /* ctrl+e, go to the end of the line */
        linenoiseEditMoveEnd(l);
        break;
    case CTRL_L: /* ctrl+l, clear screen */
        linenoiseClearScreen();
        refreshLine(l);
        break;
    case CTRL_W: /* ctrl+w, delete previous word */
        linenoiseEditDeletePrevWord(l);
        break;
    }
    flushWrite();
    xSemaphoreGive(stdout_taken_sem);
    return linenoiseEditMore;
}


/* This is part of the multiplexed linenoise API. See linenoiseEditStart()
 * for more information. This function is called when linenoiseEditFeed()
 * returns something different than NULL. At this point the user input
 * is in the buffer, and we can restore the terminal in normal mode. */
void linenoiseEditStop(struct linenoiseState *l) {
    xSemaphoreTake(stdout_taken_sem, portMAX_DELAY);
    fputc('\n', stdout);
    flushWrite();
    xSemaphoreGive(stdout_taken_sem);
}

/* This just implements a blocking loop for the multiplexed API.
 * In many applications that are not event-drivern, we can just call
 * the blocking linenoise API, wait for the user to complete the editing
 * and return the buffer. */
static char *linenoiseBlockingEdit(struct linenoiseState *l)
{
    /* Editing without a buffer is invalid. */
    if (l->buf == NULL) {
        errno = EINVAL;
        return NULL;
    }
    char *res;
    l->buflen = max_cmdline_length;
    linenoiseEditStart(l);
    // ReSharper disable once CppPossiblyErroneousEmptyStatements
    while ((res = linenoiseEditFeed(l)) == linenoiseEditMore);
    linenoiseEditStop(l);
    return res;
}

int linenoiseProbe() {
    xSemaphoreTake(stdout_taken_sem, portMAX_DELAY);
    /* Switch to non-blocking mode */
    const int stdin_fileno = fileno(stdin);
    int flags = fcntl(stdin_fileno, F_GETFL);
    flags |= O_NONBLOCK;
    int res = fcntl(stdin_fileno, F_SETFL, flags);
    if (res != 0) {
        xSemaphoreGive(stdout_taken_sem);
        return -1;
    }
    /* Device status request */
    fprintf(stdout, "\x1b[5n");
    flushWrite();

    /* Try to read response */
    int timeout_ms = 500;
    size_t read_bytes = 0;
    while (timeout_ms > 0 && read_bytes < 4) {
        const int retry_ms = 10;
        // response is ESC[0n or ESC[3n
        usleep(retry_ms * 1000);
        timeout_ms -= retry_ms;
        char c;
        const int cb = read(stdin_fileno, &c, 1);
        if (cb < 0) {
            continue;
        }
        if (read_bytes == 0 && c != '\x1b') {
            /* invalid response */
            break;
        }
        read_bytes += cb;
    }
    /* Restore old mode */
    flags &= ~O_NONBLOCK;
    res = fcntl(stdin_fileno, F_SETFL, flags);
    if (res != 0) {
        xSemaphoreGive(stdout_taken_sem);
        return -1;
    }
    if (read_bytes < 4) {
        xSemaphoreGive(stdout_taken_sem);
        return -2;
    }
    xSemaphoreGive(stdout_taken_sem);
    return 0;
}

/* The high level function that is the main API of the linenoise library. */
char *linenoise(const char *prompt, struct linenoiseState **ls_to_pass) {
    struct linenoiseState *l = *ls_to_pass;
    char *buf = calloc(1, max_cmdline_length);
    l->prompt = prompt;
    l->buf = buf;
    char *retval = linenoiseBlockingEdit(l);
    free(buf);
    return retval;
}

/* This is just a wrapper the user may want to call in order to make sure
 * the linenoise returned buffer is freed with the same allocator it was
 * created with. Useful when the main program is using an alternative
 * allocator. */
void linenoiseFree(void *ptr) {
    if (ptr == linenoiseEditMore) return; // Protect from API misuse.
    free(ptr);
}

/* ================================ History ================================= */

void linenoiseHistoryFree() {
    if (history) {
        for (int j = 0; j < history_len; j++) {
            free(history[j]);
        }
        free(history);
    }
    history = NULL;
    history_len = 0;
}

/* This is the API call to add a new entry in the linenoise history.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char *line) {

    if (history_max_len == 0) return 0;

    /* Initialization on first call. */
    if (history == NULL) {
        history = malloc(sizeof(char*)*history_max_len);
        if (history == NULL) return 0;
        memset(history,0,(sizeof(char*)*history_max_len));
    }

    /* Don't add duplicated lines. */
    if (history_len && !strcmp(history[history_len-1], line)) return 0;

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    char* linecopy = strdup(line);
    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history,history+1,sizeof(char*)*(history_max_len-1));
        history_len--;
    }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int linenoiseHistorySetMaxLen(int len) {

    if (len < 1) return 0;
    if (history) {
        int tocopy = history_len;

        char** new = malloc(sizeof(char*) * len);
        if (new == NULL) return 0;

        /* If we can't copy everything, free the elements we'll not use. */
        if (len < tocopy) {

            for (int j = 0; j < tocopy-len; j++) free(history[j]);
            tocopy = len;
        }
        memset(new,0,sizeof(char*)*len);
        memcpy(new,history+(history_len-tocopy), sizeof(char*)*tocopy);
        free(history);
        history = new;
    }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(const char *filename) {

    FILE* fp = fopen(filename, "w");
    if (fp == NULL) return -1;
    for (int j = 0; j < history_len; j++)
        fprintf(fp,"%s\n",history[j]);
    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        return -1;
    }
    char *buf = calloc(1, max_cmdline_length);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    while (fgets(buf, max_cmdline_length, fp) != NULL) {
        char* p = strchr(buf, '\r');
        if (!p) p = strchr(buf,'\n');
        if (p) *p = '\0';
        linenoiseHistoryAdd(buf);
    }
    free(buf);
    fclose(fp);
    return 0;
}

/* Set line maximum length. If len parameter is smaller than
 * LINENOISE_MINIMAL_MAX_LINE, -1 is returned
 * otherwise 0 is returned. */
int linenoiseSetMaxLineLen(size_t len) {
    if (len < LINENOISE_MINIMAL_MAX_LINE) {
        return -1;
    }
    max_cmdline_length = len;
    return 0;
}
