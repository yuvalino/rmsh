#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <sys/types.h>

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

static int utf8_rsize(unsigned char *s, size_t len) {
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
// Prompt
/////////////

#define VT_CURSTR "\033[s"  // save cursor position
#define VT_CURLDR "\033[u"  // restore cursor position
#define VT_CUREOL "\033[K"  // clear from cursor to end of line
#define VT_CURFWD "\033[C"  // move cursor forward
#define VT_CURBCK "\033[D"  // move cursor backward

struct prompt {
    char   prmt_buf[0x1000];
    char  *prmt_ptr;
    size_t prmt_cnt;
    size_t prmt_cap;
    size_t prmt_cur;

    char   prmt_u8char[4];
    size_t prmt_u8curr;
    size_t prmt_u8size;
};

void prompt_reset(struct prompt *p) {
    if (p->prmt_ptr)
        free(p->prmt_ptr);
    memset(p, 0, sizeof(*p));
    p->prmt_u8size = 1;
}

void prompt_resetchar(struct prompt *p) {
    p->prmt_u8curr = 0;
}

char *prompt_get(struct prompt *p) {
    if (!p->prmt_cnt)
        return NULL;
    
    return (p->prmt_ptr ?: p->prmt_buf);
}

int prompt_add(struct prompt *p, int c) {
    int u8sz = utf8_size(c);
    if (-1 == u8sz)
        return 1;
    
    if (u8sz == 1) {
        p->prmt_u8size = 1;
        p->prmt_u8char[0] = c;
        p->prmt_u8curr = 0;
    } else {
        if (!p->prmt_u8curr) {
            if (!u8sz)
                return -1; // continuation while not in mid-character
            p->prmt_u8size = u8sz;
            p->prmt_u8char[0] = c;
            p->prmt_u8curr = 1;
        } else {
            if (u8sz)
                return -1; // not continuation while in mid-character
            p->prmt_u8char[p->prmt_u8curr++] = c;
        }

        if (p->prmt_u8curr < p->prmt_u8size)
            return 0;
        
        p->prmt_u8curr = 0; // reset curr + null terminator
    }
    
    char *s = (p->prmt_ptr ?: p->prmt_buf);
    if (!p->prmt_ptr && (p->prmt_cnt + p->prmt_u8size) > (sizeof(p->prmt_buf) - 1)) {
        p->prmt_cap = sizeof(p->prmt_buf) * 2;
        if (!(s = p->prmt_ptr = malloc(p->prmt_cap)))
            return 1;
        memcpy(s, p->prmt_buf, sizeof(p->prmt_buf));
    }
    else if (p->prmt_ptr && (p->prmt_cnt + p->prmt_u8size) > (p->prmt_cap - 1)) {
        p->prmt_cap *= 2;
        if (!(s = p->prmt_ptr = realloc(p->prmt_ptr, p->prmt_cap)))
            return 1;
    }

    memmove(s + p->prmt_cur + p->prmt_u8size, s + p->prmt_cur, p->prmt_cnt - p->prmt_cur + 1);
    memcpy(s + p->prmt_cur, p->prmt_u8char, p->prmt_u8size);

    printf(VT_CURSTR VT_CUREOL "%s" VT_CURLDR VT_CURFWD, s + p->prmt_cur);

    p->prmt_cur += p->prmt_u8size;
    p->prmt_cnt += p->prmt_u8size;
    return 0;
}

void prompt_del(struct prompt *p) {
    if (!p->prmt_cnt)
        return;
    
    char *s = p->prmt_ptr ?: p->prmt_buf;
    int del = utf8_rsize((unsigned char *)s, p->prmt_cur);
    if (!del)
        return;
    
    memmove(s + p->prmt_cur - del, s + p->prmt_cur, p->prmt_cnt - p->prmt_cur + 1);
    p->prmt_cur -= del;
    p->prmt_cnt -= del;

    printf(VT_CURBCK VT_CURSTR VT_CUREOL "%s" VT_CURLDR, s + p->prmt_cur);
}

void prompt_seekl(struct prompt *p) {
    if (!p->prmt_cnt)
        return;
    
    char *s = p->prmt_ptr ?: p->prmt_buf;
    int cnt = utf8_rsize((unsigned char *)s, p->prmt_cur);
    if (!cnt)
        return;
    
    if (p->prmt_cur < cnt)
        cnt = p->prmt_cur;
    
    p->prmt_cur -= cnt;

    printf(VT_CURBCK);
}

void prompt_seekr(struct prompt *p) {
    if (p->prmt_cur == p->prmt_cnt)
        return;
    
    char *s = p->prmt_ptr ?: p->prmt_buf;
    int cnt = utf8_size(s[p->prmt_cur]);
    if (!cnt)
        return;
    
    if ((p->prmt_cur + cnt) > p->prmt_cnt)
        cnt = p->prmt_cnt - p->prmt_cur;
    
    p->prmt_cur += cnt;

    printf(VT_CURFWD);

}

#define GETCHAR(C) ASSERT_PERROR(EOF != (C = getchar()), "getchar");
#define ECHO_CNTRL(C) printf("^%c", 'A'+C-1)

#define PRMT_EXIT ((void *)-1)
#define PRMT_ABRT ((void *)-2)

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

/**
 * 
 */
static char *prompt(struct termios *termios_p, struct prompt *p)
{
    char *ret = PRMT_ABRT;
    struct termios raw_termios;

    // set terminal to raw mode
    memcpy(&raw_termios, termios_p, sizeof(raw_termios));
    raw_termios.c_iflag &= ~(IXON);
    raw_termios.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    ASSERT_PERROR(tcsetattr(STDIN_FILENO, TCSADRAIN, &raw_termios) == 0, "tcsetattr");

retry:
    prompt_reset(p);

    // print prompt
    char *ps1 = getenv("PS1");
    if (ps1)
        fputs(ps1, stdout);
    else if (!getuid())
        fputs("# ", stdout);
    else
        fputs("$ ", stdout);

    while (1)
    {
        int c;
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

        if (c == BACKSPACE) {
            prompt_del(p);
            continue;
        }

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
                            {printf("\nhome\n");goto retry;}
                        else if (c3 == '3')
                            {printf("\ndel\n");goto retry;}
                        else if (c3 == '4')
                            {printf("\nend\n");goto retry;}
                        else if (c3 == '5')
                            {printf("\npgup\n");goto retry;}
                        else if (c3 == '6')
                            {printf("\npgdn\n");goto retry;}
                        else if (c3 == '7')
                            {printf("\nhome\n");goto retry;}
                        else if (c3 == '8')
                            {printf("\nend\n");goto retry;}
                    }
                }
                else if (c3 == 'A')
                    {printf("\nup\n");goto retry;}
                else if (c3 == 'B')
                    {printf("\ndown\n");goto retry;}
                else if (c3 == 'D')
                    goto left;
                else if (c3 == 'C')
                    goto right;
                else if (c3 == 'H')
                    {printf("\nhome\n");goto retry;}
                else if (c3 == 'F')
                    {printf("\nend\n");goto retry;}
            }
            else if (c2 == 'O') {
                int c3;
                GETCHAR(c3);
                if (c3 == 'H')
                    {printf("\nhome\n");goto retry;}
                else if (c3 == 'F')
                    {printf("\nend\n");goto retry;}

            }
        }
        
        if (!iscntrl(c)) {
            prompt_add(p, c);
        }

        continue;

    left:
        prompt_seekl(p);
        continue;
    right:
        prompt_seekr(p);
        continue;
    }
    putchar('\n');

    ret = prompt_get(p);

out:
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
        char *in = prompt(&termios, &prmt);
        if (!in)
            continue;
        if (PRMT_EXIT == in)
            break;
        if (PRMT_ABRT == in)
            goto out;
        printf("echo ");
        fwrite(in, prmt.prmt_cnt, 1, stdout);
        printf("\n");
        printf("cnt: %zu, cur: %zu\n", prmt.prmt_cnt, prmt.prmt_cur);
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