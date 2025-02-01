#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <locale.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#define ASSERT(Condition) do { if (!(Condition)) { perror(Error); exit(1); } } while (0)
#define ASSERT_PERROR(Condition, Error) do { if (!(Condition)) { perror(Error); goto out; } } while (0)

/////////////
// UTF-8
/////////////

static int utf8_size(unsigned char c) {
    if (c <= 0x7f)
        return 1;
    if ((c & 0xc0) == 0x80)
        return 0; // continuation
    if ((c & 0xe0) == 0xc0)
        return 2;
    if ((c & 0xf0) == 0xe0)
        return 3;
    if ((c & 0xf8) == 0xf0)
        return 4;
    return -1;
}

/**
 * returns non-negative length on success or 0 on error.
 */
static int utf8_rsize(const unsigned char *s, size_t len) {
    size_t curr;
    for (curr = len - 1; curr != -1; curr--) {
        if ((s[curr] & 0xc0) != 0x80) // continuation
            break;
    }

    if (curr == -1)
        return 0;

    return len - curr;
}

/**
 * returns -1 on decoding error or zero/positive size.
 */
static ssize_t utf8_strnlen(const char *c, size_t n) {
    size_t len = 0;
    while (n && *c) {
        int u8sz = utf8_size(*c);
        if (u8sz < 1)
            return -1; // continuation byte or invalid utf8
        
        if (u8sz > n)
            return -1; // out-of-bounds
        
        for (int i = 1; i < u8sz; i++)
            if ((c[i] &0xc0) != 0x80)
                return -1; // not a continuation byte
        
        c += u8sz;
        n -= u8sz;
        len++;
    }

    return len;
}

static ssize_t utf8_strlen(const char *c) {
    return utf8_strnlen(c, (size_t)-1);
}

/////////////
// Helper functions
/////////////

/**
 * resolves the full path of a command from the path environment variable.
 * special: the command must be executable by the user.
 * returns the full path on success, or null if the command is not found.
 *         the caller is responsible for freeing the returned memory.
 */
static char *resolve_command_path(const char *command) {
    char *path = getenv("PATH");
    if (!path) 
        return NULL;

    struct stat sb;
    const char *start = path;
    const char *end;

    while (start && *start) {
        // find the next ':' or end of the string
        end = strchr(start, ':');
        if (!end) 
            end = start + strlen(start);

        // build the directory path
        size_t dir_len = end - start;
        if (dir_len > 0 && dir_len < PATH_MAX - 1) {
            char dir[PATH_MAX];
            strncpy(dir, start, dir_len);
            dir[dir_len] = '\0';

            // calculate the required size for full_path
            size_t full_path_len = dir_len + strlen(command) + 2; // 1 for '/' and 1 for '\0'
            char *full_path = malloc(full_path_len);
            if (!full_path) 
                return NULL;

            // combine directory and command
            snprintf(full_path, full_path_len, "%s/%s", dir, command);

            // check if the file exists and is executable
            // NOTE: if PATH=/a:/b and command=c and /a/c is NOT executable but /b/c is,
            //       we will choose /a/c and fail
            if (stat(full_path, &sb) == 0) {
                return full_path;
            }

            free(full_path); // free if not found
        }

        // move to the next directory
        start = (*end == ':') ? end + 1 : NULL;
    }

    return NULL;
}

void panic0()
{
    write(STDERR_FILENO, "rmsh: panic\n", 12);
    _exit(1);
}

void panic1(const char *err)
{
    write(STDERR_FILENO, "rmsh: panic: ", 13);
    write(STDERR_FILENO, err, strlen(err));
    write(STDERR_FILENO, "\n", 1);
    _exit(1);
}

void panic2(const char *err1, const char *err2)
{
    write(STDERR_FILENO, "rmsh: panic: ", 13);
    write(STDERR_FILENO, err1, strlen(err1));
    write(STDERR_FILENO, ": ", 2);
    write(STDERR_FILENO, err2, strlen(err2));
    write(STDERR_FILENO, "\n", 1);
    _exit(1);
}

int atol_exact(const char *str, int base, long *out)
{
    char *endp = (char *)str;

    // only way to find errors, meme API...
    errno = 0;
    *out = strtol(str, &endp, base);

    if (errno || *endp)
        return 1;
    return 0;
}

/**
 * returns 1 if `str` (counted by `length`) is a valid name or 0 if it isn't.
 * a valid name follows the following rules:
 *  - starts with alphabet or underscore char
 *  - contains only alphabet, numeric or underscore chars
 */
int is_valid_name(const char *str, size_t length)
{
    if (!str || length == 0) // null or empty string is invalid
        return 0;

    if (!isalpha(str[0]) && str[0] != '_') // must start with a letter
        return 0;

    for (size_t i = 1; i < length; i++) {
        if (!isalnum(str[i]) && str[i] != '_')
            return 0;
    }

    return 1;
}

/////////////
// History
/////////////

#define HIST_MAX 512
static char *history_buf[HIST_MAX] = {0};
static size_t history_cur = 0;

static int history_add(const char *line) {
    if (history_buf[history_cur])
        free(history_buf[history_cur]);
    const char *result = history_buf[history_cur++] = strdup(line);
    if (history_cur >= HIST_MAX)
        history_cur = 0;
    return !result;
}

static const char *history_get(size_t idx) {
    if (idx >= HIST_MAX)
        return NULL;
    if (idx + 1 <= history_cur)
        return history_buf[history_cur - (idx + 1)];
    return history_buf[HIST_MAX - ((idx + 1) - history_cur)];
}

/////////////
// Prompt
/////////////

/**
 * TODO:
 * 1. autocomplete
 * 2. multiline
 */

#define CTRL_A 0x01
#define CTRL_B 0x02
#define CTRL_C 0x03
#define CTRL_D 0x04
#define CTRL_E 0x05
#define CTRL_F 0x06
#define CTRL_L 0x0c
#define CTRL_R 0x12
#define BACKSPACE 0x7f

#define VT_SCRCLR "\e[2J" // clear screen
#define VT_CURSTR "\e7"   // save cursor position (\e[s] not supported by apple terminal)
#define VT_CURLDR "\e8"   // restore cursor position (\e[u] not supported by apple terminal)
#define VT_CUREOL "\e[K"  // clear from cursor to end of line
#define VT_CURFWD "\e[C"  // move cursor forward
#define VT_CURBCK "\e[D"  // move cursor backward
#define VT_CURFWD_N "\e[%dC" // move cursor N times forward
#define VT_CURBCK_N "\e[%dD" // move cursor N times backward
#define VT_CURSET_R "\e[%dd" // move cursor to row R
#define VT_CURSET_C "\e[%dG" // move cursor to column C
#define VT_CURSET_R_C "\e[%d;%dH"  // move cursor to row R column C

#define PRMT_EXIT ((void *)-1)
#define PRMT_ABRT ((void *)-2)

#define PRMT_SRCH_TEXT   "(reverse-search)`': "
#define PRMT_SRCH_TLEN   (sizeof(PRMT_SRCH_TEXT)-1)
#define PRMT_SRCH_QSTART (PRMT_SRCH_TLEN-3)

struct prompt {
    const char *prmt_ps1;

    char  *prmt_line[HIST_MAX+1];
    size_t prmt_cur_row; // 0 is current line
    size_t prmt_cur_col;

    char   *prmt_srch_line;
    size_t  prmt_srch_line_sz;
    size_t  prmt_srch_query_sz;
};

static void __prompt_reset(struct prompt *p, const char *ps1) {
    for (int i = 0; i < (HIST_MAX+1); i++)
        if (p->prmt_line[i])
            free(p->prmt_line[i]);
    if (p->prmt_srch_line)
        free(p->prmt_srch_line);
    memset(p, 0, sizeof(*p));
    p->prmt_ps1 = ps1;
}

static const char *__prompt_get(struct prompt *p, size_t idx) {
    if (idx >= (1+HIST_MAX))
        return NULL;
    return p->prmt_line[idx] ?: (idx ? history_get(idx - 1) : NULL);
}

static const char *prompt_get(struct prompt *p) {
    return __prompt_get(p, p->prmt_cur_row);
}

/**
 * returns 0 on succes and adjusts `out_moves` by the amount of moves required
 * returns -1 on error
 * NOTE: this modifies the internal state of `p`, including cursor position
 */
static int __prompt_search(struct prompt *p, size_t start_idx, const void *needle, size_t needle_len, int *out_moves) {
    const char *s, *f;
    size_t n;

    size_t idx;
    size_t pos;
    int found = 0;

    for (size_t i = start_idx; i < (1+HIST_MAX); i++) {
        s = __prompt_get(p, i);
        n = (s ? strlen(s) : 0);

        if (n < needle_len)
            continue;
        
        f = (const char *)memmem(s, n, needle, needle_len);
        if (!f)
            continue;
        
        idx = i;
        pos = ((size_t)f) - (size_t)s;
        found = 1;
        break;
    }

    // not found, just return success without doing anything
    if (!found)
        return 0;

    // find utf8-aware cursor position for the previous result and next result (so we know how to move it)
    ssize_t nextlen = utf8_strnlen(s, pos);
    ssize_t prevlen = utf8_strnlen(__prompt_get(p, p->prmt_cur_row), p->prmt_cur_col);
    if (prevlen == -1 || nextlen == -1)
        return -1; // invalid cursor position

    // replace old result with new result in search line, and make sure is null terminated
    if (!(p->prmt_srch_line = realloc(p->prmt_srch_line, PRMT_SRCH_TLEN + p->prmt_srch_query_sz + n + 1))) // +1 for \0
        return -1;
    memcpy(p->prmt_srch_line + PRMT_SRCH_TLEN + p->prmt_srch_query_sz, s, n);
    p->prmt_srch_line[PRMT_SRCH_TLEN + p->prmt_srch_query_sz + n] = 0;
    
    p->prmt_cur_row = idx;
    p->prmt_cur_col = pos;
    *out_moves += nextlen - prevlen;
    return 0;
}

static int prompt_is_search(struct prompt *p) {
    return !!(p->prmt_srch_line);
}

#define GETCHAR(C) do { C = getchar(); ASSERT_PERROR(EOF != C || errno == EINTR, "getchar"); } while (C == EOF)
#define ECHO_CNTRL(C) printf("^%c", 'A'+C-1)

static int debug_prompt(const struct termios *termios_p)
{
    int ret = 1;
    struct termios raw_termios;
    memcpy(&raw_termios, termios_p, sizeof(raw_termios));
    raw_termios.c_iflag &= ~(IXON);
    raw_termios.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    ASSERT_PERROR(tcsetattr(STDIN_FILENO, TCSADRAIN, &raw_termios) == 0, "tcsetattr");

    while (1) {
        int c;
        GETCHAR(c);
        if (iscntrl(c))
            printf("\\0%x %d\n", c, c);
        else
            printf("\\0%x %d '%c'\n", c, c, c);
        if (c == CTRL_D) {
            ret = 0;
            goto out;
        }
    }

out:
    tcsetattr(STDIN_FILENO, TCSADRAIN, termios_p);
    return ret;
}

static int prompt_winch = 0;

static void prompt_winch_sighandler(int signum, siginfo_t * siginfo, void * ucontext) {
    prompt_winch = 1;
}

enum {
    TCHTYPE_UNK = 0,
    TCHTYPE_TEXT,
    TCHTYPE_CTRL,
};

enum {
    TCHCTRL_UNK = 0,

    TCHCTRL_LINEKILL,
    TCHCTRL_EXIT,
    TCHCTRL_CLEAR,
    TCHCTRL_ENTER,
    TCHCTRL_TAB,
    TCHCTRL_SEARCH,

    TCHCTRL_DEL,
    TCHCTRL_BACKSPACE,

    TCHCTRL_HOME,
    TCHCTRL_END,
    TCHCTRL_BCKWARD,
    TCHCTRL_FORWARD,

    TCHCTRL_UP,
    TCHCTRL_DN,
    TCHCTRL_PGUP,
    TCHCTRL_PGDN,
};

#define TCHSET_CTRL(TermCharPtr, Value) do { (TermCharPtr)->tch_type = TCHTYPE_CTRL; (TermCharPtr)->tch_ctrl.value = (Value); } while (0)

struct __termchar {
    uint8_t       tch_type; // TCHT_*
    union {
        struct {
            char data[5];
            uint8_t in;
            uint8_t sz;
        } tch_text;
        struct {
            struct {
                char data[4];
                uint8_t count;
            } private;
            uint8_t value;
        } tch_ctrl;
    };
};

/**
 * returns: 1 on success, 0 if needs to read more or -1 on failure
 */
static int __termchar_input(struct __termchar *termchar, int c)
{
    if (termchar->tch_type == TCHTYPE_UNK) {
        if (c == '\e') {
            termchar->tch_type = TCHTYPE_CTRL;
            termchar->tch_ctrl.private.count = 0;
            return 0; // read more data
        }
        if (c == CTRL_A) {
            TCHSET_CTRL(termchar, TCHCTRL_HOME);
            return 1;
        }
        if (c == CTRL_B) {
            TCHSET_CTRL(termchar, TCHCTRL_BCKWARD);
            return 1;
        }
        if (c == CTRL_C) {
            TCHSET_CTRL(termchar, TCHCTRL_LINEKILL);
            return 1;
        }
        if (c == CTRL_D) {
            TCHSET_CTRL(termchar, TCHCTRL_EXIT);
            return 1;
        }
        if (c == CTRL_E) {
            TCHSET_CTRL(termchar, TCHCTRL_END);
            return 1;
        }
        if (c == CTRL_F) {
            TCHSET_CTRL(termchar, TCHCTRL_FORWARD);
            return 1;
        }
        if (c == CTRL_R) {
            TCHSET_CTRL(termchar, TCHCTRL_SEARCH);
            return 1;
        }
        if (c == CTRL_L) {
            TCHSET_CTRL(termchar, TCHCTRL_CLEAR);
            return 1;
        }
        if (c == '\n') {
            TCHSET_CTRL(termchar, TCHCTRL_ENTER);
            return 1;
        }
        if (c == '\t') {
            TCHSET_CTRL(termchar, TCHCTRL_TAB);
            return 1;
        }
        if (c == BACKSPACE) {
            TCHSET_CTRL(termchar, TCHCTRL_BACKSPACE);
            return 1;
        }
        
        if (iscntrl(c))
            return -1; // unknown control character
        
        termchar->tch_type = TCHTYPE_TEXT;
        termchar->tch_text.data[0] = c;
        termchar->tch_text.in = 1;

        if ((termchar->tch_text.sz = utf8_size(c)) < 1)
            return -1; // invalid utf8 starting char (either invalid or continuation
        
        if (termchar->tch_text.sz == 1)
            return 1; // got a single-sized character, can exit
        
        return 0; // need more chars
    }

    if (termchar->tch_type == TCHTYPE_TEXT) {
        if (termchar->tch_text.in >= termchar->tch_text.sz)
            return -1; // cannot read anymore characters
        
        termchar->tch_text.data[termchar->tch_text.in++] = c;

        if (termchar->tch_text.in == termchar->tch_text.sz) {
            termchar->tch_text.data[termchar->tch_text.sz] = 0;
            return 1; // finished reading
        }

        return 0; // `in < sz`, need more data
    }

    if (termchar->tch_type != TCHTYPE_CTRL)
        return -1; // unknown tch_type
    
    // tch_type == TCHTYPE_CTRL

    if (termchar->tch_ctrl.private.count == 0) {
        if (c != '[' && c != 'O')
            return -1; // invalid leading escape character
        termchar->tch_ctrl.private.data[termchar->tch_ctrl.private.count++] = c;
        return 0; // need more chars
    }

    if (termchar->tch_ctrl.private.data[0] == 'O') {
        if (termchar->tch_ctrl.private.count != 1)
            return -1; // may only have one more char after '\e0'
        
        if (c == 'H') {
            termchar->tch_ctrl.value = TCHCTRL_HOME;
            return 1;
        }

        if (c == 'F') {
            termchar->tch_ctrl.value = TCHCTRL_END;
            return 1;
        }

        return -1; // invalid control char after '\eO'
    }

    if (termchar->tch_ctrl.private.data[0] != '[')
        return -1; // invalid control char '\e'
    
    // termchar->tch_ctrl.private.data[0] == '['


    if (termchar->tch_ctrl.private.count == 1) {
        if (c >= '0' && c <= '9') {
            termchar->tch_ctrl.private.data[termchar->tch_ctrl.private.count++] = c;
            return 0; // numeric requires ending ~
        }

        if (c == 'A') {
            termchar->tch_ctrl.value = TCHCTRL_UP;
            return 1;
        }
        if (c == 'B') {
            termchar->tch_ctrl.value = TCHCTRL_DN;
            return 1;
        }
        if (c == 'C') {
            termchar->tch_ctrl.value = TCHCTRL_FORWARD;
            return 1;
        }
        if (c == 'D') {
            termchar->tch_ctrl.value = TCHCTRL_BCKWARD;
            return 1;
        }
        if (c == 'H') {
            termchar->tch_ctrl.value = TCHCTRL_HOME;
            return 1;
        }
        if (c == 'F') {
            termchar->tch_ctrl.value = TCHCTRL_END;
            return 1;
        }

        return -1; // invalid char after '\e['
    }

    if (termchar->tch_ctrl.private.count != 2)
        return -1; // invalid amount of chars after '\e[%c'
    
    if (c != '~')
        return -1; // invalid char after '\e[%c'
    
    // termchar->tch_ctrl.private.count == 2 && c == '~'

    c = termchar->tch_ctrl.private.data[1];
    if (c == '1') {
        termchar->tch_ctrl.value = TCHCTRL_HOME;
        return 1;
    }
    if (c == '3') {
        termchar->tch_ctrl.value = TCHCTRL_DEL;
        return 1;
    }
    if (c == '4') {
        termchar->tch_ctrl.value = TCHCTRL_END;
        return 1;
    }
    if (c == '5') {
        termchar->tch_ctrl.value = TCHCTRL_PGUP;
        return 1;
    }
    if (c == '6') {
        termchar->tch_ctrl.value = TCHCTRL_PGDN;
        return 1;
    }
    if (c == '7') {
        termchar->tch_ctrl.value = TCHCTRL_HOME;
        return 1;
    }
    if (c == '8') {
        termchar->tch_ctrl.value = TCHCTRL_END;
        return 1;
    }

    return -1; // invalid character between '\e[' and '~'
}

static void __print_movecursor(int moves)
{
    if (moves > 0)
        printf(VT_CURFWD_N, moves);
    else if (moves < 0)
        printf(VT_CURBCK_N, -moves);
}

/**
 * if `buf=NULL`, same as `__print_movecursor(moves);`
 * reprints the entire current line and moves the cursor afterwards
 */
static void __print_redrawline(const char *ps1, const char *buf, int moves)
{
    if (!buf) {
        __print_movecursor(moves);
        return;
    }
    
    if (!moves)
        printf(VT_CURSTR VT_CURSET_C "%s%s" VT_CUREOL VT_CURLDR, 1, (ps1 ?: ""), buf);
    else if (moves > 0)
        printf(VT_CURSTR VT_CURSET_C "%s%s" VT_CUREOL VT_CURLDR VT_CURFWD_N, 1, (ps1 ?: ""), buf, moves);
    else
        printf(VT_CURSTR VT_CURSET_C "%s%s" VT_CUREOL VT_CURLDR VT_CURBCK_N, 1, (ps1 ?: ""), buf, -moves);
}

/**
 * reprints line and sets cursor to end of line.
 */
static void __print_redrawline_eol(const char *ps1, const char *buf)
{
    printf(VT_CURSET_C "%s%s" VT_CURSTR VT_CUREOL VT_CURLDR, 1, (ps1 ?: ""), (buf ?: ""));
}

/**
 * if `buf=NULL`, same as `__print_movecursor(moves_before + moves_after);`
 * reprints data from cursor onwards to end of line.
 * if moves is positive, moves cursor AFTER redrawing.
 * if moves is negative, moves cursor BEFORE redrawing.
 */
static void __print_redrawcursor(const char *buf, int moves_before, int moves_after)
{
    if (!buf) {
        __print_movecursor(moves_before + moves_after);
        return;
    }

    char moves_before_s[48] = {0};
    char moves_after_s[48] = {0};

    if (moves_before > 0)
        sprintf(moves_before_s, VT_CURFWD_N, moves_before);
    else if (moves_before < 0)
        sprintf(moves_before_s, VT_CURBCK_N, -moves_before);
    
    if (moves_after > 0)
        sprintf(moves_after_s, VT_CURFWD_N, moves_after);
    else if (moves_after < 0)
        sprintf(moves_after_s, VT_CURBCK_N, -moves_after);

    printf("%s" VT_CURSTR VT_CUREOL "%s" VT_CURLDR "%s", moves_before_s, buf, moves_after_s);
}

/**
 * returns 0 on success and non-zero on failure.
 * NOTE: prints to screen.
 */
static int __prompt_output_search(struct prompt *p, const char *s, size_t n)
{
    int moves = utf8_strnlen(s, n);
    if (moves == -1)
        return -1;
    
    // do nothing for empty string
    if (!moves)
        return -1;
    
    if (!(p->prmt_srch_line = realloc(p->prmt_srch_line, p->prmt_srch_line_sz + n + 1))) // 1 for \0
        return -1;

    // make room for new data in query
    //  s = "text", n = 4
    //  before: (reverse-search)`my': mydataisnice 
    //  after:  (reverse-search)`my@@@@': mydataisnice 

    memmove(p->prmt_srch_line + PRMT_SRCH_QSTART + p->prmt_srch_query_sz + n,
            p->prmt_srch_line + PRMT_SRCH_QSTART + p->prmt_srch_query_sz,
            p->prmt_srch_line_sz - (PRMT_SRCH_QSTART + p->prmt_srch_query_sz));

    // copy new data
    memcpy(p->prmt_srch_line + PRMT_SRCH_QSTART + p->prmt_srch_query_sz,
           s,
           n);
    
    // put null terminator and update line size
    p->prmt_srch_line[p->prmt_srch_line_sz + n] = 0;
    p->prmt_srch_line_sz += n;
    p->prmt_srch_query_sz++;

    if (0 != __prompt_search(p, 0, p->prmt_srch_line + PRMT_SRCH_QSTART, p->prmt_srch_query_sz, &moves))
        return -1; // general failure while searching
    
    __print_redrawline(NULL, p->prmt_srch_line, moves);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen.
 */
static int __prompt_output_line(struct prompt *p, const char *s, size_t n)
{
    int moves = utf8_strnlen(s, n);
    if (moves == -1)
        return -1;
    
    // do nothing for empty string
    if (!moves)
        return -1;
    
    // if the current line is null and it isn't the initial line,
    // it means we're on a history line, we want to copy it to the prompt lines
    // because we never want to modify history.
    const char *hist_line;
    char *curr_line = p->prmt_line[p->prmt_cur_row];
    if (!curr_line && p->prmt_cur_row && (hist_line = history_get(p->prmt_cur_row - 1))) // `-1` because prmt_line is `1+HIST_MAX`
        if (!(curr_line = p->prmt_line[p->prmt_cur_row] = strdup(hist_line)))
            return -1;

    // resize line to be able to fit new data
    size_t curr_line_sz = (curr_line ? strlen(curr_line) : 0);
    curr_line = p->prmt_line[p->prmt_cur_row] = realloc(curr_line, curr_line_sz + n + 1); // 1 for \0
    if (!curr_line)
        return -1;
    
    // make room for new data, copy data to there and then put a null terminator
    memmove(curr_line + p->prmt_cur_col + n, curr_line + p->prmt_cur_col, curr_line_sz - p->prmt_cur_col);
    memcpy(curr_line + p->prmt_cur_col, s, n);
    curr_line[curr_line_sz + n] = 0;

    __print_redrawcursor(curr_line + p->prmt_cur_col, 0, moves);
    p->prmt_cur_col += n;

    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen.
 */
static int __prompt_output_enter_search(struct prompt *p)
{
    if (p->prmt_srch_line)
        return 0; // already in search, ignore
    
    const char *curr_line = __prompt_get(p, p->prmt_cur_row);
    size_t curr_line_sz = (curr_line ? strlen(curr_line) : 0);

    // initialize search line with searchbar and current line, put null terminator at the end
    if (!(p->prmt_srch_line = malloc(PRMT_SRCH_TLEN + curr_line_sz + 1))) // +1 for \0
        return -1;
    memcpy(p->prmt_srch_line, PRMT_SRCH_TEXT, PRMT_SRCH_TLEN);
    memcpy(p->prmt_srch_line + PRMT_SRCH_TLEN, curr_line, curr_line_sz);
    p->prmt_srch_line[PRMT_SRCH_TLEN + curr_line_sz] = 0;
    p->prmt_srch_line_sz = PRMT_SRCH_TLEN + curr_line_sz;
    p->prmt_srch_query_sz = 0;

    // calculate cursor difference
    int moves = utf8_strlen(p->prmt_ps1);
    if (-1 == moves)
        return -1;
    moves = ((ssize_t)PRMT_SRCH_TLEN) - moves;

    __print_redrawline(NULL, p->prmt_srch_line, moves);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen.
 */
static int __prompt_output_next_search(struct prompt *p)
{
    if (!p->prmt_srch_line)
        return -1; // not in search mode

    size_t prev_row = p->prmt_cur_row;
    size_t prev_col = p->prmt_cur_col;
    int moves = 0;
    if (0 != __prompt_search(p, p->prmt_cur_row + 1, p->prmt_srch_line + PRMT_SRCH_QSTART, p->prmt_srch_query_sz, &moves))
        return -1; // general failure while searching
    
    // not found, do nothing
    if (moves == 0 && prev_row == p->prmt_cur_row && prev_col == p->prmt_cur_col)
        return 0;
    
    __print_redrawline(NULL, p->prmt_srch_line, moves);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * If `out_moves` is NULL, redraws current line.
 * Else, amount of cursor moves is returned via `out_moves`.
 * NOTE: prints to screen.if `out_moves` is NULL.
 */
static int __prompt_output_exit_search(struct prompt *p, int *out_moves)
{
    if (!p->prmt_srch_line)
        return 0; // not in search, ignore

    // calculate total srch prompt length (not including result)
    int srch_u8len = utf8_strnlen(p->prmt_srch_line + PRMT_SRCH_QSTART, p->prmt_srch_query_sz);
    if (-1 == srch_u8len)
        return -1; // invalid utf8 search query
    srch_u8len += PRMT_SRCH_TLEN;

    // reset search params
    free(p->prmt_srch_line);
    p->prmt_srch_line = NULL;
    p->prmt_srch_line_sz = p->prmt_srch_query_sz = 0;

    // calculate cursor difference
    int moves = utf8_strlen(p->prmt_ps1);
    if (-1 == moves)
        return -1;
    moves -= srch_u8len;

    if (out_moves)
        *out_moves += moves;
    else
        __print_redrawline(p->prmt_ps1, (prompt_get(p) ?: ""), moves);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen.
 */
static int __prompt_output_backspace_search(struct prompt *p)
{
    if (!p->prmt_srch_line)
        return -1; // not in search
    
    if (!p->prmt_srch_query_sz)
        return 0; // no search, ignore

    int del = utf8_rsize((unsigned char *)(p->prmt_srch_line + PRMT_SRCH_QSTART), p->prmt_srch_query_sz);
    if (!del)
        return -1; // invalid utf8 length
    
    memmove(p->prmt_srch_line + PRMT_SRCH_QSTART + p->prmt_srch_query_sz - del,
            p->prmt_srch_line + PRMT_SRCH_QSTART + p->prmt_srch_query_sz,
            p->prmt_srch_line_sz - (PRMT_SRCH_QSTART + p->prmt_srch_query_sz) + 1); // +1 for \0
    p->prmt_srch_line_sz -= del;
    p->prmt_srch_query_sz -= del;

    __print_redrawline(NULL, p->prmt_srch_line, -1);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen.
 */
static int __prompt_output_backspace_line(struct prompt *p)
{
    if (!p->prmt_cur_col)
        return 0; // nothing to delete
    
    // if the current line is null and it isn't the initial line,
    // it means we're on a history line, we want to copy it to the prompt lines
    // because we never want to modify history.
    const char *hist_line;
    char *curr_line = p->prmt_line[p->prmt_cur_row];
    if (!curr_line && p->prmt_cur_row && (hist_line = history_get(p->prmt_cur_row - 1))) // `-1` because prmt_line is `1+HIST_MAX`
        if (!(curr_line = p->prmt_line[p->prmt_cur_row] = strdup(hist_line)))
            return -1;

    int del = utf8_rsize((unsigned char *)curr_line, p->prmt_cur_col);
    if (!del)
        return -1; // invalid utf8 length
    
    if (del > p->prmt_cur_col)
        del = p->prmt_cur_col;
    
    memmove(curr_line + p->prmt_cur_col - del, curr_line + p->prmt_cur_col, strlen(curr_line) - p->prmt_cur_col + 1); // +1 for \0
    p->prmt_cur_col -= del;

    __print_redrawcursor(curr_line + p->prmt_cur_col, -1, 0);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen if `out_moves` is NULL
 */
static int __prompt_output_del(struct prompt *p, int *out_moves)
{
    const char *curr_line_const = prompt_get(p);
    size_t n = (curr_line_const ? strlen(curr_line_const) : 0);

    if (p->prmt_cur_col >= n)
        return 0; // nothing to delete
    
    // if the current line is null and it isn't the initial line,
    // it means we're on a history line, we want to copy it to the prompt lines
    // because we never want to modify history.
    const char *hist_line;
    char *curr_line = p->prmt_line[p->prmt_cur_row];
    if (!curr_line && p->prmt_cur_row && (hist_line = history_get(p->prmt_cur_row - 1))) // `-1` because prmt_line is `1+HIST_MAX`
        if (!(curr_line = p->prmt_line[p->prmt_cur_row] = strdup(hist_line)))
            return -1;

    int del = utf8_size(curr_line[p->prmt_cur_col]);
    if (!del)
        return -1; // invalid utf8 length
    
    if (del > (n - p->prmt_cur_col))
        del = n - p->prmt_cur_col;
    
    memmove(curr_line + p->prmt_cur_col, curr_line + p->prmt_cur_col + del, strlen(curr_line) - p->prmt_cur_col + 1); // +1 for \0

    if (!out_moves)
        __print_redrawcursor(curr_line + p->prmt_cur_col, 0, 0);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen if `out_moves` is NULL.
 */
static int __prompt_output_cursor_backward(struct prompt *p, int *out_moves)
{
    if (!p->prmt_cur_col)
        return 0; // nothing to delete
    
    const char *curr_line = prompt_get(p);
    int cnt = utf8_rsize((const unsigned char *)curr_line, p->prmt_cur_col);
    if (!cnt)
        return -1; // invalid utf8 length
    
    if (p->prmt_cur_col < cnt)
        cnt = p->prmt_cur_col;
    
    p->prmt_cur_col -= cnt;

    if (out_moves)
        *out_moves -= 1;
    else
        __print_movecursor(-1);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen if `out_moves` is NULL.
 */
static int __prompt_output_cursor_forward(struct prompt *p, int *out_moves)
{
    const char *curr_line = prompt_get(p);
    size_t curr_line_sz = (curr_line ? strlen(curr_line) : 0);

    if (p->prmt_cur_col >= curr_line_sz)
        return 0; // nothing to delete
    
    int cnt = utf8_size(curr_line[p->prmt_cur_col]);
    if (cnt == 0 || cnt == -1)
        return -1;
    
    if ((p->prmt_cur_col + cnt) > curr_line_sz)
        cnt = curr_line_sz - p->prmt_cur_col;

    p->prmt_cur_col += cnt;

    if (out_moves)
        *out_moves += 1;
    else
        __print_movecursor(1);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen if `out_moves` is NULL.
 */
static int __prompt_output_cursor_home(struct prompt *p, int *out_moves)
{
    int ret;
    int moves = 0;

    while (p->prmt_cur_col)
        if ((ret = __prompt_output_cursor_backward(p, &moves)))
            return ret;

    if (out_moves)
        *out_moves += moves;
    else
        __print_movecursor(moves);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen if `out_moves` is NULL.
 */
static int __prompt_output_cursor_end(struct prompt *p, int *out_moves)
{
    int ret;
    int moves = 0;
    const char *curr_line = prompt_get(p);
    size_t curr_line_sz = (curr_line ? strlen(curr_line) : 0);

    while (p->prmt_cur_col < curr_line_sz)
        if ((ret = __prompt_output_cursor_forward(p, &moves)))
            return ret;
    if (out_moves)
        *out_moves += moves;
    else
        __print_movecursor(moves);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen.
 */
static int __prompt_output_history_up(struct prompt *p)
{
    int ignored;
    int ret;

    if (p->prmt_cur_row + 1 >= HIST_MAX + 1) // out-of-bounds
        return 0;
    
    if (!history_get(p->prmt_cur_row)) // top of history (no +1, history is HIST_MAX, prmt_lines is 1+HIST_MAX)
        return 0;

    // exit search if in search
    if (p->prmt_srch_line && (ret = __prompt_output_exit_search(p, &ignored)))
        return ret;

    p->prmt_cur_row++;
    const char *curr_line = prompt_get(p);
    p->prmt_cur_col = (curr_line ? strlen(curr_line) : 0);
    __print_redrawline_eol(p->prmt_ps1, curr_line);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen.
 */
static int __prompt_output_history_down(struct prompt *p)
{
    int ignored;
    int ret;

    if (!p->prmt_cur_row)
        return 0;

    // exit search if in search
    if (p->prmt_srch_line && (ret = __prompt_output_exit_search(p, &ignored)))
        return ret;

    p->prmt_cur_row--;
    const char *curr_line = prompt_get(p);
    p->prmt_cur_col = (curr_line ? strlen(curr_line) : 0);
    __print_redrawline_eol(p->prmt_ps1, curr_line);
    return 0;
}

/**
 * returns 0 on success and non-zero on failure
 * NOTE: prints to screen.
 */
static int __prompt_output_clear(struct prompt *p)
{
    int moves = 0;
    int ret;

    // exit search if in search
    if (p->prmt_srch_line && (ret = __prompt_output_exit_search(p, &moves)))
        return ret;

    if (moves > 0)
        printf(VT_CURFWD_N VT_CURSTR VT_SCRCLR VT_CURSET_R_C "%s%s" VT_CURLDR VT_CURSET_R, moves, 1, 1, p->prmt_ps1, (prompt_get(p) ?: ""), 1);
    else if (moves < 0)
        printf(VT_CURBCK_N VT_CURSTR VT_SCRCLR VT_CURSET_R_C "%s%s" VT_CURLDR VT_CURSET_R, -moves, 1, 1, p->prmt_ps1, (prompt_get(p) ?: ""), 1);
    else
        printf(VT_CURSTR VT_SCRCLR VT_CURSET_R_C "%s%s" VT_CURLDR VT_CURSET_R, 1, 1, p->prmt_ps1, (prompt_get(p) ?: ""), 1);
    return 0;
}

static const char *__prompt_output(struct prompt *p, struct __termchar *input)
{
    int ret;
    if (input->tch_type == TCHTYPE_TEXT) {
        ret = (p->prmt_srch_line ? __prompt_output_search : __prompt_output_line)(p, input->tch_text.data, input->tch_text.sz);
        return ret ? PRMT_ABRT : NULL;
    }

    if (input->tch_type != TCHTYPE_CTRL)
        return PRMT_ABRT;
    
    // input->tch_type == TCHTYPE_CTRL

    if (input->tch_ctrl.value == TCHCTRL_EXIT) {
        ECHO_CNTRL(CTRL_D);
        putchar('\n');
        return PRMT_EXIT;
    }
    
    if (input->tch_ctrl.value == TCHCTRL_ENTER) {
        putchar('\n');
        return (prompt_get(p) ?: ""); // can't return null because we want to reprint ps1
    }
    
    if (input->tch_ctrl.value == TCHCTRL_LINEKILL) {
        ECHO_CNTRL(CTRL_C);
        putchar('\n');
        return "";
    }

    if (input->tch_ctrl.value == TCHCTRL_SEARCH) {
        ret = (p->prmt_srch_line ? __prompt_output_next_search : __prompt_output_enter_search)(p);
        return ret ? PRMT_ABRT : NULL;
    }

    if (input->tch_ctrl.value == TCHCTRL_TAB) {
        if (p->prmt_srch_line)
            return __prompt_output_exit_search(p, NULL) ? PRMT_ABRT : NULL;
        return NULL;
    }

    if (input->tch_ctrl.value == TCHCTRL_BACKSPACE) {
        ret = (p->prmt_srch_line ? __prompt_output_backspace_search : __prompt_output_backspace_line)(p);
        return ret ? PRMT_ABRT : NULL;
    }

    if (input->tch_ctrl.value == TCHCTRL_UP)
        return __prompt_output_history_up(p) ? PRMT_ABRT : NULL;
    if (input->tch_ctrl.value == TCHCTRL_DN)
        return __prompt_output_history_down(p) ? PRMT_ABRT : NULL;

    if (input->tch_ctrl.value == TCHCTRL_CLEAR)
        return __prompt_output_clear(p) ? PRMT_ABRT : NULL;

    // from here, only line mode is compatible - everything else exits and redraws entire line

    int moves = 0;

    int (*fn)(struct prompt *, int *) = NULL;

    if      (input->tch_ctrl.value == TCHCTRL_DEL    ) fn = __prompt_output_del;
    else if (input->tch_ctrl.value == TCHCTRL_BCKWARD) fn = __prompt_output_cursor_backward;
    else if (input->tch_ctrl.value == TCHCTRL_FORWARD) fn = __prompt_output_cursor_forward;
    else if (input->tch_ctrl.value == TCHCTRL_HOME   ) fn = __prompt_output_cursor_home;
    else if (input->tch_ctrl.value == TCHCTRL_END    ) fn = __prompt_output_cursor_end;
    else
        return NULL; // ignore unknown control char

    if (!p->prmt_srch_line)
        return (fn(p, NULL) ? PRMT_ABRT : NULL);
    
    if (__prompt_output_exit_search(p, &moves) || fn(p, &moves))
        return PRMT_ABRT;
    
    __print_redrawline(p->prmt_ps1, (prompt_get(p) ?: ""), moves);
    return NULL;
}

static const char *prompt(struct prompt *p, const struct termios *termios_p)
{
    const char *ret = PRMT_ABRT;
    struct termios raw_termios;
    struct winsize winsz;
    struct sigaction winch_act, winch_oldact;
    int set_act = 0;
    
    char *ps1;

    int c;
    int moves;
    const char *buf;
    const char *draw_ps1;

    struct __termchar termchar;
    int termchar_ret;

    prompt_winch = 1;

    // set terminal to raw mode
    memcpy(&raw_termios, termios_p, sizeof(raw_termios));
    raw_termios.c_iflag &= ~(IXON);
    raw_termios.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    ASSERT_PERROR(tcsetattr(STDIN_FILENO, TCSADRAIN, &raw_termios) == 0, "tcsetattr");

    winch_act.sa_flags = SA_SIGINFO;
    winch_act.sa_sigaction = prompt_winch_sighandler;
    ASSERT_PERROR(sigaction(SIGWINCH, &winch_act, &winch_oldact) == 0, "sigaction");
    set_act = 1;

retry:
    ps1 = (getenv("PS1") ?: (getuid() ? "$ " : "# "));
    fputs(ps1, stdout);

    __prompt_reset(p, ps1);

    ret = NULL;
    while (!ret)
    {
        memset(&termchar, 0, sizeof(termchar));

        do {
            GETCHAR(c);
        }
        while (0 == (termchar_ret = __termchar_input(&termchar, c)));

        if (c == '\0')
            goto out; // EOF

        if (-1 == termchar_ret)
            continue; // ignore invalid input
        
        // termchar_ret == 1

        ret = __prompt_output(p, &termchar);
    }

out:
    if (set_act)
        sigaction(SIGWINCH, &winch_oldact, NULL);
    tcsetattr(STDIN_FILENO, TCSADRAIN, termios_p);
    return ret;
}

///////
// Lex
///////

#define LEXF_META    0x1
#define LEXF_PREMETA 0x2

/**
 * wrapper for a parsed token
 */
struct lex_tok {
    char  *s; // string value
    size_t n; // size

    int flags;

    struct lex_tok *next; // link to `lex.tok_head`
};

static void lex_tok_free(struct lex_tok *tok)
{
    if (tok->s)
        free(tok->s);
    
    // we "leak" `next` since its `struct lex` responsibility to free it
    free(tok);
}

/**
 * return token's value and free's the token itself.
 * never fails.
 */
static char *lex_tok_consume(struct lex_tok *tok)
{
    char *tmp = tok->s;
    tok->s = NULL;
    tok->n = 0;

    lex_tok_free(tok);

    return tmp;
}

struct lex {
    const char *input;
    const char *cursor;

    unsigned line;
    char *error;
    int erralloc;

    struct lex_tok *tok_head; // linked list of already parsed tokens
};

static void lex_free(struct lex *lex) {
    while (lex->tok_head) {
        struct lex_tok *tmp = lex->tok_head;
        lex->tok_head = tmp->next;

        lex_tok_free(tmp);
    }

    if (lex->erralloc && lex->error)
        free(lex->error);
    
    free(lex);
}

static int lex_create(const char *input, unsigned line, struct lex **outp) {
    int ret = 1;

    if (!(*outp = calloc(1, sizeof(**outp))))
        panic1(strerror(ENOMEM));

    (*outp)->input = (*outp)->cursor = input;
    (*outp)->line = line;

    ret = 0;

out:
    return ret;
}

static void lex_set_error(struct lex *lex, const char *fmt, ...)
{
    if (lex->error) {
        if (lex->erralloc)
            free(lex->error);
        lex->error = NULL;
        lex->erralloc = 0;
    }

    va_list ap;
    va_start(ap, fmt);
    if (-1 == vasprintf(&lex->error, fmt, ap)) {
        lex->error = strerror(ENOMEM);
        lex->erralloc = 0;
    }
    else {
        lex->erralloc = 1;
    }
        
    va_end(ap);
}

/**
 * inserts a single character to token
 */
static int lex_token_insert(struct lex *lex, struct lex_tok *tok, char input)
{
    if (!(tok->s = realloc(tok->s, tok->n + 1))) {
        lex_set_error(lex, strerror(ENOMEM));
        return 1;
    }

    tok->s[tok->n] = input;
    tok->n++;

    return 0;
}

/**
 * inserts a single character from lex->cursor to token.
 * advances lex->cursor on success.
 */
static int lex_token_insert_cursor(struct lex *lex, struct lex_tok *tok)
{
    if (0 != lex_token_insert(lex, tok, *lex->cursor))
        return 1;
    
    lex->cursor++;
    return 0;
}

static const char *IFS = " \t\n";
static const char *METACHARS = "|&;()<>";

/**
 * returns 0 with `*outp` allocated token on success.
 * if EOF was reached, the string value (as returned from lex_tok_consume) is NULL.
 * returns -1 on general error and sets lex->error
 */
static int lex_pop_token(struct lex *lex, struct lex_tok **outp)
{
#define CURR (*lex->cursor)

    int ret = 1;
    int done_ifs = 0;
    struct lex_tok *tok = NULL;
    
    // if we have a cached token, pop it and be done with
    if (lex->tok_head) {
        struct lex_tok *tmp = lex->tok_head;
        lex->tok_head = tmp->next;
        
        tmp->next = NULL;
        *outp = tmp;
        return 0;
    }

    if (!(tok = calloc(1, sizeof(*tok)))) {
        lex_set_error(lex, strerror(ENOMEM));
        goto out;
    }

    for (; CURR; lex->cursor++) {

        // parse meta characters together, split words
        if (strchr(METACHARS, CURR)) {
            // if a word is already constructing, break but mark it as PREMETA
            if (!(tok->flags & LEXF_META) && tok->n) {
                tok->flags |= LEXF_PREMETA;
                break;
            }
            
            // this is the first non-whitespace char, return it
            if (0 != lex_token_insert(lex, tok, CURR))
                goto out;
            tok->flags |= LEXF_META; // mark as meta char (if not already marked)
            continue;
        }
        
        // not a meta-character

        // if constructing a metacharactered word, make sure to cut it first
        if (tok->flags & LEXF_META)
            break;

        // lines counter
        if (CURR == '\n')
            lex->line++;

        // ifs: skip if didn't parse anything yet, else break and return whatever we parsed so far
        if (strchr(IFS, CURR)) {
            if (!done_ifs)
                continue;
            break;
        }

        done_ifs = 1;

        // handle quoted strings
        if (CURR == '\'' || CURR == '"') {
            char quote = CURR; // store the quote type
            lex->cursor++;     // move past the opening quote

            while (CURR && CURR != quote) {
                // lines counter
                if (CURR == '\n')
                    lex->line++;

                if (0 != lex_token_insert_cursor(lex, tok))
                    goto out;
            }

            if (CURR != quote) {
                // unterminated quote, cleanup and return error
                lex_set_error(lex, "unexpected EOF while looking for matching quote `%c%c", quote, (int)'"' + (int)'\'' - quote);
                goto out;
            }
            continue; // skip adding the closing quote to the token
        }

        // add to token without advancing lex->cursor (since it will be done by the for clause)
        if (0 != lex_token_insert(lex, tok, CURR))
            goto out;
    }

    // null-terminate the token, this also makes sure that input="\"\"" resolves to an empty string
    if (done_ifs) {
        if (0 != lex_token_insert(lex, tok, '\0'))
            goto out;
    }
    
    if (outp)
        *outp = tok;
    ret = 0;

out:
    if (ret || !outp)
        lex_tok_free(tok);
    
    return ret;

#undef CURR
}

/**
 * push token to lex' token "stack", so next lex_pop_token() will return it.
 */
static void lex_push_token(struct lex *lex, struct lex_tok *tok)
{
    if (tok->next)
        panic1("tok->next != NULL");
    
    tok->next = lex->tok_head;
    lex->tok_head = tok;
}

// Redirection type
#define LEXRT_PATH_IN      0x01  // [n]<word
#define LEXRT_PATH_OTRUNC  0x02  // [n]>word
#define LEXRT_PATH_OAPPEND 0x04  // [n]>>word
#define LEXRT_PATH_INOUT   0x08  // [n]<>word
#define LEXRT_FD_IN        0x10  // [n]<&word
#define LEXRT_FD_OUT       0x20  // [n]>&word

#define LEXRT_PATH         (LEXRT_PATH_IN | LEXRT_PATH_OTRUNC | LEXRT_PATH_OAPPEND | LEXRT_PATH_INOUT)
#define LEXRT_FD           (LEXRT_FD_IN   | LEXRT_FD_OUT)

struct lex_redir {
    struct lex_redir *next; // link to `lex_proc->redir_head`
    
    int redir_fd;  // target fd number to redirect

    int type; // input / output / appendput

    union {
        char *s;
        int   fd;
    } source;
};

struct lex_proc {
    int    argc;
    char **argv;
    int    envc;
    char **envp;

    struct lex_redir *redir_head;
    struct lex_redir *redir_tail;
};

static void free_lex_proc(struct lex_proc *p) {

    if (!p)
        return;
    
    while (p->redir_head) {
        struct lex_redir *tmp = p->redir_head;
        p->redir_head = tmp->next;

        free(tmp);
    }
    p->redir_tail = NULL;

    if (p->envp) {
        for (int i = 0; i < p->envc; i++)
            free(p->envp[i]);
        free(p->envp);
    }

    if (p->argv) {
        for (int i = 0; i < p->argc; i++)
            free(p->argv[i]);
        free(p->argv);
    }

    free(p);
}

/**
 * returns 0 and sets `*outp` to allocated `lex_proc` struct.
 * returns -1 on general error and sets `lex->error`.
 */
static int lex_pop_proc(struct lex *lex, struct lex_proc **outp)
{
    int ret = -1;
    int done_vars = 0;  // whether we've finished parsing env vars
    struct lex_proc *p = NULL;
    struct lex_tok *tok = NULL;
    struct lex_tok *premeta = NULL;

    if (!(p = calloc(1, sizeof(*p)))) {
        lex_set_error(lex, strerror(ENOMEM));
        goto out;
    }

    while (lex->tok_head || *lex->cursor) {
        if (0 != lex_pop_token(lex, &tok))
            goto out;

        // handle meta characters
        if (tok->flags & LEXF_META) {
            if (!tok->n || !tok->s)
                panic1("metachar tok empty");

            // redirection
            if (strchr("<>", tok->s[0])) {
                // default redirect FDs
                long parsed_fd = ((tok->s[0] == '>') ? STDOUT_FILENO : STDIN_FILENO);

                // parse redirect fd from premeta token if available
                if (premeta) {
                    if (0 != atol_exact(premeta->s, 10, &parsed_fd) || parsed_fd < 0) {
                        // if premeta token is invalid file descriptor, reparse it token as regular token
                        lex_push_token(lex, tok);
                        tok = NULL;
                        lex_push_token(lex, premeta);
                        premeta = NULL;
                        continue;
                    }

                    lex_tok_free(premeta);
                    premeta = NULL;
                }

                // allocate new redir into p->redir_tail
                struct lex_redir *redir;
                if (!(redir = calloc(1, sizeof(*redir)))) {
                    lex_set_error(lex, strerror(ENOMEM));
                    goto out;
                }
                redir->redir_fd = (int)parsed_fd;
                if (!p->redir_head)
                    p->redir_head = redir;
                else
                    p->redir_tail->next = redir;
                p->redir_tail = redir;

                // parse redirection type
                if (strcmp(tok->s, "<") == 0)
                    redir->type = LEXRT_PATH_IN;
                else if (strcmp(tok->s, ">") == 0)
                    redir->type = LEXRT_PATH_OTRUNC;
                else if (strcmp(tok->s, ">>") == 0)
                    redir->type = LEXRT_PATH_OAPPEND;
                else if (strcmp(tok->s, "<>") == 0)
                    redir->type = LEXRT_PATH_INOUT;
                else if (strcmp(tok->s, "<&") == 0)
                    redir->type = LEXRT_FD_IN;
                else if (strcmp(tok->s, ">&") == 0)
                    redir->type = LEXRT_FD_OUT;
                else {
                    lex_set_error(lex, "unknown redirection op `%s'", tok->s);
                    goto out;
                }

                // get source redirection arg (free current tok)
                lex_tok_free(tok);
                tok = NULL;

                if (0 != lex_pop_token(lex, &tok))
                    goto out;
                
                if (!tok->s) {
                    lex_set_error(lex, "syntax error near unexpected EOF");
                    goto out;
                }

                if (tok->flags & LEXF_META) {
                    lex_set_error(lex, "syntax error near unexpected token `%s'", tok->s);
                    goto out;
                }

                if (redir->type & LEXRT_FD) {
                    // parse as file descriptor
                    if (0 != atol_exact(tok->s, 10, &parsed_fd) || parsed_fd < 0) {
                        lex_set_error(lex, "invalid redirection fd `%s'", tok->s);
                        goto out;
                    }
                    lex_tok_free(tok);
                    tok = NULL;
                }
                else {
                    // parse as raw string
                    redir->source.s = lex_tok_consume(tok);
                    tok = NULL;
                }
            }
            else {
                // unknown metacharacter, we'll skip it, maybe our caller can handle it ... ?
                lex_push_token(lex, tok);
                tok = NULL;

                // ... unless we have a premeta token, push it (its PREMETA flag was stripped) and re-parse it
                if (premeta) {
                    lex_push_token(lex, premeta);
                    premeta = NULL;
                    continue;
                }

                break;
            }

            // having premeta token here just doesn't make sense, we just handled a meta token
            if (premeta)
                panic2("unattended premeta token", premeta->s);

            // parse next token after metacharacter is parsed
            continue;
        }
        
        if (tok->flags & LEXF_PREMETA) {
            if (premeta)
                panic2("premeta tokens encountered twice, first", premeta->s);
            
            // save premeta token for parsing meta character
            premeta = tok;
            tok = NULL;

            // turn off premeta flag
            premeta->flags &= ~LEXF_PREMETA;
            continue;
        }
        else if (premeta) {
            lex_push_token(lex, tok);
            tok = premeta;
            premeta = NULL;
        }

        char *arg = lex_tok_consume(tok);
        tok = NULL;
        if (!arg)
            continue;

        // parse envp (before argv)
        if (!done_vars) {
            char *env_name_end = strchr(arg, '=');
            if (env_name_end && is_valid_name(arg, (size_t)env_name_end - (size_t)arg)) {
                // add token to envp
                if (!(p->envp = realloc(p->envp, (p->envc + 1) * sizeof(char *)))) {
                    lex_set_error(lex, strerror(ENOMEM));
                    free(arg);
                    goto out;
                }
                p->envp[p->envc++] = arg;
                continue;
            }
        }

        done_vars = 1;

        // add token to argv
        if (!(p->argv = realloc(p->argv, (p->argc + 1) * sizeof(char *)))) {
            lex_set_error(lex, strerror(ENOMEM));
            free(arg);
            goto out;
        }
        p->argv[p->argc++] = arg;
    }

    // add NULL at the end of argv
    if (!(p->argv = realloc(p->argv, (p->argc + 1) * sizeof(char *)))) {
        lex_set_error(lex, strerror(ENOMEM));
        goto out;
    }
    p->argv[p->argc++] = NULL;

    // add NULL at the end of argv
    if (p->envp) {
        if (!(p->envp = realloc(p->envp, (p->envc + 1) * sizeof(char *)))) {
            lex_set_error(lex, strerror(ENOMEM));
            goto out;
        }
        p->envp[p->envc++] = NULL;
    }

    *outp = p;
    ret = 0;
    
out:
    if (premeta)
        lex_tok_free(premeta);
    if (tok)
        lex_tok_free(tok);
    if (ret)
        free_lex_proc(p);
    return ret;
}

struct lex_pipeline {
    size_t            proclen;
    struct lex_proc **procarr;
};

static void free_lex_pipeline(struct lex_pipeline *p)
{
    if (!p)
        return;
    
    if (p->procarr) {
        for (int i = 0; i < p->proclen; i++)
            free_lex_proc(p->procarr[i]);
        free(p->procarr);
    }

    free(p);
}

static int lex_pop_pipeline(struct lex *lex, struct lex_pipeline **outp)
{
    int ret = -1;
    struct lex_pipeline *p = NULL;
    struct lex_proc *next_proc = NULL;
    struct lex_tok *tok = NULL;

    if (!(p = calloc(1, sizeof(*p)))) {
        lex_set_error(lex, strerror(ENOMEM));
        goto out;
    }

    while (1) {
        // parse one process
        if (0 != lex_pop_proc(lex, &next_proc))
            goto out;

        if (!(p->procarr = realloc(p->procarr, (p->proclen + 1) * sizeof(*p->procarr)))) {
            lex_set_error(lex, strerror(ENOMEM));
            goto out;
        }

        p->procarr[p->proclen++] = next_proc;
        next_proc = NULL;

        // see if we get a pipe character so we continue to parse
        if (0 != lex_pop_token(lex, &tok))
            goto out;
        
        // EOF
        if (!tok->s)
            break;
        
        // doesn't make any sense
        if ((tok->flags & LEXF_META) == 0)
            panic2("expected metacharacter", tok->s);
        
        // if not a pipeline, push token back and return - maybe caller knows how to parse it?
        if (0 != strcmp(tok->s, "|")) {
            // TODO when wish to not throw error
            lex_set_error(lex, "unexpected metacharacter `%s'", tok->s);
            goto out;
            // lex_push_token(lex, tok);
            // tok = NULL;
            // break;
        }

        lex_tok_free(tok);
        tok = NULL;

        // make sure we have not reached EOF immediately after parsing pipe character
        if (0 != lex_pop_token(lex, &tok))
            goto out;
        if (!tok->s) {
            lex_set_error(lex, "syntax error: unexpected end of file");
            goto out;
        }

        // push back the popped token, it belongs to proc parser and we stole it :(
        lex_push_token(lex, tok);
        tok = NULL;
    }

    *outp = p;
    ret = 0;
    
out:
    if (tok)
        lex_tok_free(tok);
    if (next_proc)
        free_lex_proc(next_proc);
    if (ret)
        free_lex_pipeline(p);
    return ret;
}

/////////////
// Interpreter
/////////////

/**
 * TODO:
 * . pipelines
 * . lists
 * .. && ||
 * .. ; <newline>
 * . compound commands
 * .. (list)
 * .. { list; }
 * .. ((expression))
 * .. [[ expression ]]
 * . parameters
 * . special parameters
 * . arrays
 * . expansion
 * .. brace
 * .. tilde
 * .. parameter
 * .. command substitution
 * .. arithmetic
 * .. word splitting
 * .. pathname
 * .. pattern matching
 * . aliases
 * . functions
 * 
 */

struct rmsh {
    const char *shname;
    struct termios termios;  // saved term attrs
    pid_t pgid;  // -1 if not interactive, else shell pgid
};

#define RMSH_STRERR(Sh, Errno) fprintf(stderr, "%s: %s\n", (Sh)->shname, strerror(Errno))
#define RMSH_SYSERR(Sh) RMSH_STRERR((Sh), errno)

#define RMSH_ERRMSG(Sh, Msg) fprintf(stderr, "%s: %s\n", (Sh)->shname, (Msg))
#define RMSH_STRERRMSG(Sh, Errno, Msg) fprintf(stderr, "%s: %s: %s\n", (Sh)->shname, (Msg), strerror(Errno))
#define RMSH_SYSERRMSG(Sh, Msg) RMSH_STRERRMSG((Sh), errno, (Msg))

#define RMSH_ERRFMT(Sh, Fmt, ...) fprintf(stderr, "%s: " Fmt "\n", (Sh)->shname, ##__VA_ARGS__)
#define RMSH_STRERRFMT(Sh, Errno, Fmt, ...) fprintf(stderr, "%s: " Fmt ": %s\n", (Sh)->shname, ##__VA_ARGS__, strerror(Errno))
#define RMSH_SYSERRFMT(Sh, Fmt, ...) RMSH_STRERRFMT((Sh), errno, Fmt, ##__VA_ARGS__)

static int rmsh_open(const char *shname, int interactive, struct rmsh *out_sh)
{
    memset(out_sh, 0, sizeof(*out_sh));
    out_sh->shname = shname;
    out_sh->pgid = -1;

    if (interactive) {
        out_sh->pgid = getpgid(0);
        if (out_sh->pgid <= 0) {
            fprintf(stderr, "%s: getpgid(0): %s\n", shname, strerror(errno));
            return 1;
        }
    }

    return 0;
}

static void rmsh_close(struct rmsh *sh)
{

}

struct rmsh_proc {
    struct rmsh_proc *next;
    struct lex_proc *lex;
    pid_t pid;
    int return_status; // -1 if not yet completed
    int return_signal; // signal of termination, 0 if exited
};

static void free_rmsh_proc(struct rmsh_proc *p) {
    if (p->lex)
        free_lex_proc(p->lex);
    free(p);
}

#define REDIR_MODE 0666

/**
 * child function
 */
static int rmsh_child(const char *shname, struct rmsh_proc *p, int infile, int outfile, pid_t pgid)
{
    int tmpfd;
    char *exe_path = NULL;

    // pgid != -1 essentially means an interactive shell
    // NOTE: this MUST appear before restoring default signal handlers!! `tcsetpgrp` with unignored SIGTTOU causes it to be raised and stop the process
    if (pgid != -1) {
        if (pgid == 0)
            pgid = getpid();
        
        // put the process into the process group and give the process group
        // the terminal, if appropriate. this has to be done both by the shell and in the individual
        // child processes because of potential race conditions
        if (0 != setpgid(0, pgid)) {
            int tmperrno = errno;
            if (pgid != getpgid(0)) {
                fprintf(stderr, "%s: setpgid(0, %d): %s\n", shname, pgid, strerror(tmperrno));
                goto out;
            }
        }

        // TODO: only for foreground, currently only foreground is supported
        if (infile == STDIN_FILENO) {
            // put process group to terminal foreground
            if (pgid != tcgetpgrp(infile)) {
                if (0 != tcsetpgrp(infile, pgid)) {
                    fprintf(stderr, "%s: tcsetpgrp(%d, %d): %s\n", shname, infile, pgid, strerror(errno));
                    goto out;
                }
            }
        }
    }

    // set the handling for job control signals back to the default
    struct sigaction sa = { .sa_handler = SIG_DFL };
    if (0 != sigaction(SIGINT, &sa, NULL)) {
        fprintf(stderr, "%s: sigaction: %s\n", shname, strerror(errno));
        goto out;
    }
    if (0 != sigaction(SIGQUIT, &sa, NULL)) {
        fprintf(stderr, "%s: sigaction: %s\n", shname, strerror(errno));
        goto out;
    }
    if (0 != sigaction(SIGTSTP, &sa, NULL)) {
        fprintf(stderr, "%s: sigaction: %s\n", shname, strerror(errno));
        goto out;
    }
    if (0 != sigaction(SIGTTIN, &sa, NULL)) {
        fprintf(stderr, "%s: sigaction: %s\n", shname, strerror(errno));
        goto out;
    }
    if (0 != sigaction(SIGTTOU, &sa, NULL)) {
        fprintf(stderr, "%s: sigaction: %s\n", shname, strerror(errno));
        goto out;
    }
    if (0 != sigaction(SIGCHLD, &sa, NULL)) {
        fprintf(stderr, "%s: sigaction: %s\n", shname, strerror(errno));
        goto out;
    }

    // set the standard input/output of the new process
    if (infile != STDIN_FILENO)
    {
        if (-1 == dup2(infile, STDIN_FILENO)) {
            fprintf(stderr, "%s: dup2(stdin): %s\n", shname, strerror(errno));
            goto out;
        }
        if (-1 == close(infile)) {
            fprintf(stderr, "%s: close: %s\n", shname, strerror(errno));
            goto out;
        }
    }
    if (outfile != STDOUT_FILENO)
    {
        if (-1 == dup2(outfile, STDOUT_FILENO)) {
            fprintf(stderr, "%s: dup2(stdin): %s\n", shname, strerror(errno));
            goto out;
        }
        if (-1 == close(outfile)) {
            fprintf(stderr, "%s: close: %s\n", shname, strerror(errno));
            goto out;
        }
    }

    // do redirections
    for (struct lex_redir *redir = p->lex->redir_head; redir; redir = redir->next) {
        // open/get fd by type
        if (redir->type == LEXRT_PATH_IN)
            tmpfd = open(redir->source.s, O_RDONLY, REDIR_MODE);
        else if (redir->type == LEXRT_PATH_OTRUNC)
            tmpfd = open(redir->source.s, O_CREAT | O_WRONLY | O_TRUNC, REDIR_MODE);
        else if (redir->type == LEXRT_PATH_OAPPEND)
            tmpfd = open(redir->source.s, O_CREAT | O_WRONLY | O_APPEND, REDIR_MODE);
        else if (redir->type == LEXRT_PATH_INOUT)
            tmpfd = open(redir->source.s, O_CREAT | O_RDWR, REDIR_MODE); // NOTE: bash does not trunc, 
        else if (redir->type & LEXRT_FD)
            tmpfd = redir->source.fd;
        else
            panic1("invalid redirection");

        // handle errors
        if (-1 == tmpfd) {
            if (redir->type & LEXRT_PATH)
                fprintf(stderr, "%s: %s: %s\n", shname, redir->source.s, strerror(errno));
            else
                fprintf(stderr, "%s: redirection to %d failed\n", shname, redir->redir_fd);
            goto out;
        }

        // if already target fd, just don't do anything
        if (tmpfd == redir->redir_fd)
            continue;
        
        // in the end, all redirections are just a dup
        close(redir->redir_fd);
        if (redir->redir_fd != fcntl(tmpfd, F_DUPFD, redir->redir_fd)) {
            fprintf(stderr, "%s: dup: %s\n", shname, strerror(errno));
            goto out;
        }

        // if we used open, close it
        if (redir->type & LEXRT_PATH)
            close(tmpfd);
    }

    // resolve argv0 to exe path
    if (strchr(p->lex->argv[0], '/')) {
        if (!(exe_path = strdup(p->lex->argv[0]))) {
            fprintf(stderr, "%s: %s\n", shname, strerror(ENOMEM));
            goto out;
        }
    }
    else if ((exe_path = resolve_command_path(p->lex->argv[0]))) {
    }
    else {
        fprintf(stderr, "%s: %s: command not found\n", shname, p->lex->argv[0]);
        goto out;
    }

    // add env vars to current process
    if (p->lex->envp) {
        for (char **env = p->lex->envp; *env; env++)
            putenv(*env);
    }

    execv(exe_path, p->lex->argv);
    fprintf(stderr, "%s: %s: %s\n", shname, exe_path, strerror(errno));

out:
    if (exe_path)
        free(exe_path);
    // cleanup for tvm TODO: solve in tvm-level
    if (p)
        free_rmsh_proc(p);
    
    return 1;
}

/**
 * consumes ownership of `p` on success only!
 */
static int rmsh_launch_proc(struct rmsh *sh, struct lex_proc *lp, int infile, int outfile, pid_t pgid, struct rmsh_proc **outp)
{
    int ret = -1;
    struct rmsh_proc *p = NULL;

    // allocate proc struct and move lexp's ownership to it
    if (!(p = calloc(1, sizeof(*p)))) {
        RMSH_STRERR(sh, ENOMEM);
        goto out;
    }

    p->return_status = -1;

    if (-1 == (p->pid = fork())) {
        RMSH_SYSERRMSG(sh, "fork");
        goto out;
    }

    // from here we can't fail, moves ownership to proc
    p->lex = lp;
    lp = NULL;

    if (0 == p->pid) {
        _exit(rmsh_child(sh->shname, p, infile, outfile, pgid));
    }
    
    *outp = p;
    ret = 0;
out:
    if (ret)
        free_rmsh_proc(p);
    if (lp)
        free_lex_proc(lp);
    return ret;
}

struct rmsh_job {
    struct rmsh_proc *proc_head;
    struct rmsh_proc *proc_tail;
    pid_t pgid;
};

void free_rmsh_job(struct rmsh_job *j)
{
    if (!j)
        return;
    
    while (j->proc_head) {
        struct rmsh_proc *tmp = j->proc_head;
        j->proc_head = tmp->next;
        free_rmsh_proc(tmp);
    }

    free(j);
}

int wait_rmsh_job(struct rmsh *sh, struct rmsh_job *j)
{
    int ret = -1;
    pid_t pid;
    int status;
    struct rmsh_proc *p;
    int uncompleted_procs = 0;

    // count amount of uncompleted procs
    for (p = j->proc_head; p; p = p->next) {
        if (p->return_status == -1)
            uncompleted_procs++;
    }

    // wait for all job's processes
    while (uncompleted_procs) {
        // wait for next proces to exit
        int status;
        if (-1 == (pid = wait(&status))) {
            fprintf(stderr, "%s: waitpid: %s\n", sh->shname, strerror(errno));
            goto out;
        }

        // find processes
        for (p = j->proc_head; p; p = p->next) {
            if (p->pid == pid)
                break;
        }
        if (!p) {
            fprintf(stderr, "%s: waitpid: unknown pid %d\n", sh->shname, pid);
            goto out;
        }

        // save exit status
        if (WIFEXITED(status))
            p->return_status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) {
            p->return_status = 128 + WTERMSIG(status);
            p->return_signal = WTERMSIG(status);
        }
        else {
            fprintf(stderr, "%s: waitpid: undefined status 0x%08x\n", sh->shname, status);
            goto out;
        }

        // one more processes has completed running
        uncompleted_procs--;
    }

    // HACK: when the kernel delivers SIGINT to the foreground job of a terminal
    //       due to Ctrl+C, it also prints ^C without a trailing newline.
    //       so print a newline to compensate for the kernel.
    if (sh->pgid != -1 && j->proc_head != NULL && j->proc_tail->return_signal == SIGINT)
        putchar('\n');

    ret = 0;
out:
    return ret;
}

static int rmsh_launch_job(struct rmsh *sh, struct lex_pipeline *p, struct rmsh_job **outj)
{
    int ret = -1;
    struct rmsh_job *j = NULL;
    int iopipe[2] = {-1, -1};
    int infile = STDIN_FILENO;
    int outfile = -1;

    if (!(j = calloc(1, sizeof(*j)))) {
        RMSH_STRERR(sh, ENOMEM);
        goto out;
    }

    // -1 means not interactive
    j->pgid = ((sh->pgid != -1) ? 0 : -1);

    // create and launch procs
    for (int i = 0; i < p->proclen; i++) {
        // set up pipes, if necessary
        if (i < (p->proclen - 1))
        {
            if (0 != pipe(iopipe)) {
                RMSH_STRERRMSG(sh, ENOMEM, "pipe");
                goto out;
            }
            outfile = iopipe[1];
            iopipe[1] = -1;
        }
        else
            outfile = STDOUT_FILENO;

        // fork the child process
        struct rmsh_proc *pp;
        if (0 != rmsh_launch_proc(sh, p->procarr[i], infile, outfile, j->pgid, &pp)) {
            goto out;
        }

        p->procarr[i] = NULL; // ownership was moved to `pp`, make sure we don't free twice

        // put proc in job
        if (j->proc_head) {
            j->proc_tail->next = pp;
            j->proc_tail = pp;
        }
        else {
            j->proc_head = j->proc_tail = pp;
        }

        // if interactive shell, make sure we update job's pgid and put new process in correct process pgroup
        if (sh->pgid != -1) {
            if (!j->pgid)
                j->pgid = pp->pid;
            setpgid(pp->pid, j->pgid);  // NOTE: no error checking, we do this twice (also in child)
        }

        // clean up after pipes
        if (infile != STDIN_FILENO && infile != -1) {
            close(infile);
            infile = -1;
        }
        if (outfile != STDOUT_FILENO && outfile != -1) {
            close(outfile);
            outfile = -1;
        }
        infile = iopipe[0];
        iopipe[0] = -1;
    }

    *outj = j;
    ret = 0;

out:
    if (iopipe[0] != -1)
        close(iopipe[0]);
    if (iopipe[1] != -1)
        close(iopipe[1]);
    if (infile != STDIN_FILENO && infile != -1)
        close(infile);
    if (outfile != STDOUT_FILENO && outfile != -1)
        close(outfile);
    if (ret)
        free_rmsh_job(j);
    free_lex_pipeline(p);
    return ret;
}

static int rmsh_input(struct rmsh *sh, const char *input)
{
    int ret = -1;
    int status;
    struct lex *lex;
    struct lex_pipeline *p = NULL;
    struct rmsh_job *j = NULL;

    if (0 != lex_create(input, 1, &lex))
        goto out;

    if (0 != lex_pop_pipeline(lex, &p)) {
        RMSH_ERRFMT(sh, "line %u: %s", lex->line, lex->error ? : strerror(0));
        goto out;
    }

    if (0 != rmsh_launch_job(sh, p, &j))
        goto out;

    if (0 != wait_rmsh_job(sh, j))
        goto out;

    // put shell back in terminal foreground
    if (sh->pgid != -1) {
        if (0 != tcsetpgrp(STDIN_FILENO, sh->pgid)) {
            fprintf(stderr, "%s: tcsetpgrp(0, %d): %s\n", sh->shname, sh->pgid, strerror(errno));
            goto out;
        }

        if (0 != tcsetattr(STDIN_FILENO, TCSADRAIN, &sh->termios)) {
            fprintf(stderr, "%s: tcsetattr(0): %s\n", sh->shname, strerror(errno));
            goto out;
        }
    }

    ret = 0;
out:
    if (lex)
        lex_free(lex);
    return ret;
}

/////////////
// Main
/////////////

static int interactive(const char *shname, int debug_input) {
    int ret = 1;
    struct prompt prmt = {0};
    struct rmsh sh;

    if (0 != rmsh_open(shname, 1, &sh))
        goto out;

    // loop until we are in the foreground
    while (1) {
        pid_t pgid = tcgetpgrp(STDIN_FILENO);
        ASSERT_PERROR(pgid > 0, "tcgetpgrp");
        if (pgid == sh.pgid)
            break;
        kill(0, SIGTTIN);
    }

    // ignore interactive and job-control signals
    struct sigaction sa = { .sa_handler = SIG_IGN };
    ASSERT_PERROR(sigaction(SIGINT, &sa, NULL) == 0, "sigaction");
    ASSERT_PERROR(sigaction(SIGQUIT, &sa, NULL) == 0, "sigaction");
    ASSERT_PERROR(sigaction(SIGTSTP, &sa, NULL) == 0, "sigaction");
    ASSERT_PERROR(sigaction(SIGTTIN, &sa, NULL) == 0, "sigaction");
    ASSERT_PERROR(sigaction(SIGTTOU, &sa, NULL) == 0, "sigaction");

    // usually set to SIG_IGN too, but right now we don't want it to be like that
    sa.sa_handler = SIG_DFL;
    ASSERT_PERROR(sigaction(SIGCHLD, &sa, NULL) == 0, "sigaction");

    // take control of the terminal and get attributes
    setpgid(0, 0); // no checks, if we're session leader, it'll fail with EPERM
    ASSERT_PERROR(tcsetpgrp(STDIN_FILENO, sh.pgid) == 0, "tcsetpgrp");
    ASSERT_PERROR(tcgetattr(STDIN_FILENO, &sh.termios) == 0, "tcgetattr");

    if (debug_input) {
        debug_prompt(&sh.termios);
        goto out;
    }

    while (1) {
        const char *in = prompt(&prmt, &sh.termios);
        if (!in)
            continue;
        if (PRMT_EXIT == in)
            break;
        if (PRMT_ABRT == in)
            goto out;
        if (!strlen(in))
            continue;
        
        if (0 != history_add(in))
            goto out;
        
        // we don't care, let use know and continue
        (void)rmsh_input(&sh, in);
    }

    ret = 0;
out:

    rmsh_close(&sh);

    return ret;
}

static int noninteractive(const char *shname, const char *command) {
    int ret = 1;
    struct rmsh sh = {0};

    if (0 != rmsh_open(shname, 0, &sh))
        goto out;

    if (0 != rmsh_input(&sh, command))
            goto out;

    ret = 0;
out:

    rmsh_close(&sh);

    return ret;
}

static void helpexit(const char *exe)
{
    printf("USAGE: %s [OPTION]...\n", exe);
    printf("rmsh shell\n\n");
    printf("  -c COMMAND     run a single command and exit\n");
    printf("  -D             run debug input mode\n");
    printf("  -h             display this help and exit\n");
    exit(0);
}

#ifdef LIBRMSH
int rmsh_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    int debug_input = 0;
    const char *bname = strrchr(argv[0], '/');
    bname = (bname ? (bname + 1) : argv[0]);

    const char *command = NULL;

    int c;
    do {
        c = getopt(argc, argv, "hc:D");\

        if (c == 'h') {
            helpexit(bname);
        }
        else if (c == 'c') {
            command = optarg;
        }
        else if (c == 'D') {
            debug_input = 1;
        }
        else {
            if (c == -1) {
                if (!argv[optind])
                    break;
                fprintf(stderr, "%s: invalid argument '%s'\n", bname, argv[optind]);
            }
            fprintf(stderr, "Try '%s -h' for more information.\n", bname);
            exit(1);
        }
    } while (c >= 0);

    if (command)
        return noninteractive(bname, command);

    if (isatty(STDIN_FILENO))
        return interactive(bname, debug_input);
    
    char *cmdbuf = NULL;
    size_t cmdn = 0;
    while (1) {
        char chunk[4096] = {0};
        ssize_t currn = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (currn < 0) {
            perror(bname);
            free(cmdbuf);
            return 1;
        }
        if (!currn)
            break;
        if (!(cmdbuf = realloc(cmdbuf, cmdn + currn))) {
            errno = ENOMEM;
            perror(bname);
            return 1;
        }
        memcpy(cmdbuf + cmdn, chunk, currn);
    }

    int ret = noninteractive(bname, cmdbuf);
    free(cmdbuf);
    return ret;
}