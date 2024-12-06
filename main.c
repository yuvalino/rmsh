#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <locale.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#define ASSERT_PERROR(Condition, Error) do { if (!(Condition)) { perror(Error); goto out; } } while (0)

#define CTRL_A 0x01
#define CTRL_B 0x02
#define CTRL_C 0x03
#define CTRL_D 0x04
#define CTRL_L 0x0c
#define CTRL_R 0x12
#define BACKSPACE 0x7f

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

/////////////
// History
/////////////

#define HIST_MAX 512
static char *history_buf[HIST_MAX] = {0};
static size_t history_cur = 0;

int history_add(const char *line) {
    if (history_buf[history_cur])
        free(history_buf[history_cur]);
    const char *result = history_buf[history_cur++] = strdup(line);
    if (history_cur >= HIST_MAX)
        history_cur = 0;
    return !result;
}

const char *history_get(size_t idx) {
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
 * 2. search history
 * 3. autocomplete
 */

#define VT_SCRCLR "\e[2J" // clear screen
#define VT_CURSTR "\e7"   // save cursor position (\e[s] not supported by apple terminal)
#define VT_CURLDR "\e8"   // restore cursor position (\e[u] not supported by apple terminal)
#define VT_CUREOL "\e[K"  // clear from cursor to end of line
#define VT_CURFWD "\e[C"  // move cursor forward
#define VT_CURBCK "\e[D"  // move cursor backward
#define VT_CURFWD_N "\e[%dC" // move cursor N times forward
#define VT_CURBCK_N "\e[%dD" // move cursor N times backward
#define VT_CURSET_R "\e[%dd" // move cursor to row R
#define VT_CURSET_R_C "\e[%d;%dH"  // move cursor to row R column C

#define PRMT_EXIT ((void *)-1)
#define PRMT_ABRT ((void *)-2)

struct prompt {
    char  *prmt_line[HIST_MAX+1];
    size_t prmt_cur_row; // 0 is current line
    size_t prmt_cur_col;

    char   prmt_u8char[4];
    size_t prmt_u8curr;
    size_t prmt_u8size;
};

void prompt_reset(struct prompt *p) {
    for (int i = 0; i < (HIST_MAX+1); i++)
        if (p->prmt_line[i])
            free(p->prmt_line[i]);
    memset(p, 0, sizeof(*p));
    p->prmt_u8size = 1;
}

void prompt_resetchar(struct prompt *p) {
    p->prmt_u8curr = 0;
}

const char *prompt_get(struct prompt *p) {
    return p->prmt_line[p->prmt_cur_row] ?: (p->prmt_cur_row ? history_get(p->prmt_cur_row - 1) : NULL);
}

const char *prompt_add(struct prompt *p, int c) {
    int u8sz = utf8_size(c);
    if (-1 == u8sz)
        return PRMT_ABRT;
    if (u8sz == 1) {
        p->prmt_u8size = 1;
        p->prmt_u8char[0] = c;
        p->prmt_u8curr = 0;
    } else {
        if (!p->prmt_u8curr) {
            if (!u8sz)
                return PRMT_ABRT; // continuation while not in mid-character
            p->prmt_u8size = u8sz;
            p->prmt_u8char[0] = c;
            p->prmt_u8curr = 1;
        } else {
            if (u8sz)
                return PRMT_ABRT; // not continuation while in mid-character
            p->prmt_u8char[p->prmt_u8curr++] = c;
        }

        if (p->prmt_u8curr < p->prmt_u8size)
            return NULL;
        
        p->prmt_u8curr = 0; // reset curr + null terminator
    }

    char *s = p->prmt_line[p->prmt_cur_row];
    const char *h;
    if (!s && p->prmt_cur_row && (h = history_get(p->prmt_cur_row - 1))) {
        if (!(s = p->prmt_line[p->prmt_cur_row] = strdup(h)))
            return PRMT_ABRT;
    }

    size_t n = (s ? strlen(s) : 0);
    s = p->prmt_line[p->prmt_cur_row] = realloc(s, n + p->prmt_u8size + 1); // 1 for \0
    if (!s)
        return PRMT_ABRT;
    
    memmove(s + p->prmt_cur_col + p->prmt_u8size, s + p->prmt_cur_col, n - p->prmt_cur_col);
    memcpy(s + p->prmt_cur_col, p->prmt_u8char, p->prmt_u8size);
    s[n + p->prmt_u8size] = 0;

    p->prmt_cur_col += p->prmt_u8size;
    return s + p->prmt_cur_col - p->prmt_u8size;
}

const char *prompt_backspace(struct prompt *p) {
    if (!p->prmt_cur_col)
        return NULL;
    
    char *s = p->prmt_line[p->prmt_cur_row];
    const char *h;
    if (!s && p->prmt_cur_row && (h = history_get(p->prmt_cur_row - 1))) {
        if (!(s = p->prmt_line[p->prmt_cur_row] = strdup(h)))
            return PRMT_ABRT;
    }

    int del = utf8_rsize((unsigned char *)s, p->prmt_cur_col);
    if (!del)
        return PRMT_ABRT;
    
    if (del > p->prmt_cur_col)
        del = p->prmt_cur_col;
    
    memmove(s + p->prmt_cur_col - del, s + p->prmt_cur_col, strlen(s) - p->prmt_cur_col + 1); // +1 for \0
    p->prmt_cur_col -= del;

    return s + p->prmt_cur_col;
}

/**
 * returns char to reprint (without moving cursor), NULL if nothing to do and PRMT_ABRT on error.
 */
const char *prompt_del(struct prompt *p) {
    const char *s_const = prompt_get(p);
    size_t n = (s_const ? strlen(s_const) : 0);

    if (p->prmt_cur_col >= n)
        return NULL;
    
    char *s = p->prmt_line[p->prmt_cur_row];
    const char *h;
    if (!s && p->prmt_cur_row && (h = history_get(p->prmt_cur_row - 1))) {
        if (!(s = p->prmt_line[p->prmt_cur_row] = strdup(h)))
            return PRMT_ABRT;
    }
    
    int del = utf8_size(s[p->prmt_cur_col]);
    if (!del)
        return PRMT_ABRT;
    
    if (del > (n - p->prmt_cur_col))
        del = n - p->prmt_cur_col;
    
    memmove(s + p->prmt_cur_col, s + p->prmt_cur_col + del, n - p->prmt_cur_col + 1); // +1 for \0
    return s + p->prmt_cur_col;
}

/**
 * returns cursor backward move amount or -1 on error.
 */
int prompt_seekl(struct prompt *p) {
    if (!p->prmt_cur_col)
        return 0;
    
    const char *s = prompt_get(p);
    int cnt = utf8_rsize((const unsigned char *)s, p->prmt_cur_col);
    if (!cnt)
        return -1;
    
    if (p->prmt_cur_col < cnt)
        cnt = p->prmt_cur_col;
    
    p->prmt_cur_col -= cnt;
    return 1;
}

/**
 * returns cursor backward move amount or -1 on error.
 */
int prompt_seekr(struct prompt *p) {
    const char *s = prompt_get(p);
    size_t n = (s ? strlen(s) : 0);

    if (p->prmt_cur_col == n)
        return 0;
    
    int cnt = utf8_size(s[p->prmt_cur_col]);
    if (!cnt)
        return -1;
    
    if ((p->prmt_cur_col + cnt) > n)
        cnt = n - p->prmt_cur_col;
    
    p->prmt_cur_col += cnt;
    return 1;
}

/**
 * returns cursor backward move amount or -1 on error.
 */
int prompt_home(struct prompt *p) {
    if (!p->prmt_cur_col)
        return 0;
    
    const char *s = prompt_get(p);
    size_t moves = 0;
    size_t cur = p->prmt_cur_col;
    while (cur) {
        int u8sz = utf8_rsize((const unsigned char *)s, cur);
        if (!u8sz) {
            return -1;
        }
        
        if (u8sz > cur)
            u8sz = cur;
        cur -= u8sz;
        moves++;
    }

    p->prmt_cur_col = 0;
    return (int)moves;
}

/**
 * returns cursor forward move amount or -1 on error.
 */
int prompt_end(struct prompt *p) {
    const char *s = prompt_get(p);
    size_t n = (s ? strlen(s) : 0);

    if (p->prmt_cur_col >= n)
        return 0;

    size_t moves = 0;
    size_t cur = p->prmt_cur_col;
    while (cur < n) {
        int u8sz = utf8_size(s[cur]);
        if (!u8sz) {
            return -1;
        }
        
        if (cur + u8sz > n)
            u8sz = n - cur;
        cur += u8sz;
        moves++;
    }

    p->prmt_cur_col = n;
    return moves;
}

int prompt_up(struct prompt *p, const char **out_print) {
    *out_print = NULL;

    if (p->prmt_cur_row + 1 >= HIST_MAX + 1) // out-of-bounds
        return 0;
    
    if (!history_get(p->prmt_cur_row)) // top of history (no +1, history is HIST_MAX, prmt_lines is 1+HIST_MAX)
        return 0;
    
    int moves = prompt_home(p);
    if (-1 == moves)
        return -1;
    
    p->prmt_cur_row++;
    const char *s = prompt_get(p);
    p->prmt_cur_col = s ? strlen(s) : 0;

    *out_print = s;
    return moves;
}

int prompt_down(struct prompt *p, const char **out_print) {
    *out_print = NULL;

    if (!p->prmt_cur_row)
        return 0;
    
    int moves = prompt_home(p);
    if (-1 == moves)
        return -1;
    
    p->prmt_cur_row--;
    const char *s = prompt_get(p);
    p->prmt_cur_col = s ? strlen(s) : 0;

    *out_print = s;
    return moves;
}

#define GETCHAR(C) do { C = getchar(); ASSERT_PERROR(EOF != C || errno == EINTR, "getchar"); } while (C == EOF)
#define ECHO_CNTRL(C) printf("^%c", 'A'+C-1)

static int debug_prompt(struct termios *termios_p)
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

static const char *prompt(struct termios *termios_p, struct prompt *p)
{
    const char *ret = PRMT_ABRT;
    struct termios raw_termios;
    struct winsize winsz;
    struct sigaction winch_act, winch_oldact;
    int set_act = 0;

    int c;
    int moves;
    const char *buf;

    prompt_winch = 1;

    // set terminal to raw mode
    memcpy(&raw_termios, termios_p, sizeof(raw_termios));
    raw_termios.c_iflag &= ~(IXON);
    raw_termios.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    ASSERT_PERROR(tcsetattr(STDIN_FILENO, TCSADRAIN, &raw_termios) == 0, "tcsetattr");

    //ASSERT_PERROR(ioctl(STDIN_FILENO, TIOCGWINSZ, &winsz) == 0, "ioctl(TIOCGWINSZ)");

    winch_act.sa_flags = SA_SIGINFO;
    winch_act.sa_sigaction = prompt_winch_sighandler;
    ASSERT_PERROR(sigaction(SIGWINCH, &winch_act, &winch_oldact) == 0, "sigaction");
    set_act = 1;

retry:
    prompt_reset(p);

    // print prompt
    char *ps1 = getenv("PS1");
    if (ps1)
        {}
    else if (!getuid())
        ps1 = "# ";
    else
        ps1 = "$ ";
    fputs(ps1, stdout);

    while (1)
    {
        GETCHAR(c);
        
        if (c == CTRL_D) {
            ret = PRMT_EXIT;
            putchar('\n');
            goto out;
        }

        if (c == '\n')
            break;
        
        if (c == CTRL_C) {
            ECHO_CNTRL(c);
            putchar('\n');
            goto retry;
        }

        if (c == BACKSPACE)
            goto backspace;
        
        if (c == CTRL_L)
            goto clear_screen;

        if (c == '\033') {
            int c2;
            GETCHAR(c2);
            if (c2 == '[') {
                int c3;
                GETCHAR(c3);
                if (c3 >= '0' && c3 <= '9') {
                    int c4;
                    GETCHAR(c4);
                    if (c4 == '~') {
                        if (c3 == '1')
                            goto home;
                        else if (c3 == '3')
                            goto del;
                        else if (c3 == '4')
                            goto end;
                        else if (c3 == '5')
                            {printf("\npgup\n");goto retry;}
                        else if (c3 == '6')
                            {printf("\npgdn\n");goto retry;}
                        else if (c3 == '7')
                            goto home;
                        else if (c3 == '8')
                            goto end;
                    }
                }
                else if (c3 == 'A')
                    goto up;
                else if (c3 == 'B')
                    goto down;
                else if (c3 == 'D')
                    goto left;
                else if (c3 == 'C')
                    goto right;
                else if (c3 == 'H')
                    goto home;
                else if (c3 == 'F')
                    goto end;
            }
            else if (c2 == 'O') {
                int c3;
                GETCHAR(c3);
                if (c3 == 'H')
                    goto home;
                else if (c3 == 'F')
                    goto end;

            }
        }
        
        if (!iscntrl(c))
            goto add;

        continue;

    add:
        if (PRMT_ABRT == (buf = prompt_add(p, c)))
            goto out;
        if (buf)
            printf(VT_CURSTR VT_CUREOL "%s" VT_CURLDR VT_CURFWD, buf);
        continue;

    backspace:
        if (PRMT_ABRT == (buf = prompt_backspace(p)))
            goto out;
        if (buf)
            printf(VT_CURBCK VT_CURSTR VT_CUREOL "%s" VT_CURLDR, buf);
        continue;

    del:
        if (PRMT_ABRT == (buf = prompt_del(p)))
            goto out;
        if (buf)
            printf(VT_CURSTR VT_CUREOL "%s" VT_CURLDR, buf);
        continue;
    
    clear_screen:
        printf(VT_CURSTR VT_SCRCLR VT_CURSET_R_C "%s%s" VT_CURLDR VT_CURSET_R, 1, 1, ps1, prompt_get(p) ?: "", 1);
        continue;

    left:
        moves = prompt_seekl(p);
        goto curbck;
    home:
        moves = prompt_home(p);
    curbck:
        if (moves == -1)
            goto out;
        if (moves > 0)
            printf(VT_CURBCK_N, moves);
        continue;
    
    right:
        moves = prompt_seekr(p);
        goto curfwd;
    end:
        moves = prompt_end(p);
    curfwd:
        if (-1 == moves)
            goto out;
        if (moves > 0)
            printf(VT_CURFWD_N, moves);
        continue;
    
    up:
        moves = prompt_up(p, &buf);
        goto set_line;
    down:
        moves = prompt_down(p, &buf);
    set_line:
        if (moves > 0 && buf)
            printf(VT_CURBCK_N VT_CUREOL "%s", moves, buf);
        else if (moves > 0)
            printf(VT_CURBCK_N VT_CUREOL, moves);
        else if (buf)
            printf(VT_CUREOL "%s", buf);
        continue;
    }
    putchar('\n');

    ret = prompt_get(p);

out:
    if (set_act)
        sigaction(SIGWINCH, &winch_oldact, NULL);
    tcsetattr(STDIN_FILENO, TCSADRAIN, termios_p);
    return ret;
}

static int interactive() {
    int ret = 1;
    struct termios termios;
    pid_t shpgid;
    struct prompt prmt = {0};

    shpgid = getpgrp();
    ASSERT_PERROR(shpgid > 0, "getpgrp");

    // loop until we are in the foreground
    while (1) {
        pid_t pgid = tcgetpgrp(STDIN_FILENO);
        ASSERT_PERROR(pgid > 0, "tcgetpgrp");
        if (pgid == shpgid)
            break;
        kill(0, SIGTTIN);
    }

    // take control of the terminal and get attributes
    ASSERT_PERROR(setpgid(0, 0) == 0, "setpgid(0,0)");
    ASSERT_PERROR(tcsetpgrp(STDIN_FILENO, shpgid) == 0, "tcsetpgrp");
    ASSERT_PERROR(tcgetattr(STDIN_FILENO, &termios) == 0, "tcgetattr");

    // debug_prompt(&termios);
    // goto out;
    
    while (1) {
        const char *in = prompt(&termios, &prmt);
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
        
        printf("%s\n", in);
    }

    ret = 0;
out:
    return ret;
}

int main(int argc, char **argv)
{
    if (isatty(STDIN_FILENO))
        return interactive();
    
    fprintf(stderr, "interactive only\n");
    return 1;
}