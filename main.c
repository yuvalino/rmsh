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
// Prompt
/////////////

struct prompt {
    char  *prmt_ptr;
    char   prmt_buf[0x1000];
    size_t prmt_cnt;
    size_t prmt_cap;
    size_t prmt_cur;
};

void prompt_reset(struct prompt *p) {
    p->prmt_cur = p->prmt_cap = p->prmt_cnt = p->prmt_buf[0] = 0;
    if (p->prmt_ptr) {
        free(p->prmt_ptr);
        p->prmt_ptr = NULL;
    }
}

char *prompt_get(struct prompt *p) {
    if (!p->prmt_cnt)
        return NULL;
    
    return (p->prmt_ptr ?: p->prmt_buf);
}

int prompt_add(struct prompt *p, char c) {
    if (!p->prmt_ptr) {
        if (p->prmt_cnt < sizeof(p->prmt_buf)) {
            p->prmt_buf[p->prmt_cnt++] = c;
            putc(c, stdout);
            return 0;
        }
        
        // p->prmt_cnt == sizeof(p->prmt_buf)
        p->prmt_cap = sizeof(p->prmt_buf) * 2;
        if (!(p->prmt_ptr = malloc(p->prmt_cap)))
            return 1;
        memcpy(p->prmt_ptr, p->prmt_buf, sizeof(p->prmt_buf));
    }
    else if (p->prmt_cnt == p->prmt_cap) {
        p->prmt_cap *= 2;
        if (!(p->prmt_ptr = realloc(p->prmt_ptr, p->prmt_cap)))
            return 1;
    }

    p->prmt_ptr[p->prmt_cnt++] = c;
    putc(c, stdout);
    return 0;
}

void prompt_del(struct prompt *p) {
    if (!p->prmt_cnt)
        return;
    
    p->prmt_cnt--;

    // take UTF-16 into account
    if (p->prmt_cnt && prompt_get(p) && prompt_get(p)[p->prmt_cnt] < 0)
        p->prmt_cnt--;
    
    printf("\b \b");
}

#define GETCHAR(C) ASSERT_PERROR(EOF != (C = getchar()), "getchar");
#define ECHO_CNTRL(C) printf("^%c", 'A'+C-1)

#define ECHO_RIGHT(Times) printf("\033[%dC", (Times))
#define ECHO_LEFT(Times)  printf("\033[%dD", (Times))

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
                    {printf("\nleft\n");goto retry;}//ECHO_LEFT(1);
                else if (c3 == 'C')
                    {printf("\nright\n");goto retry;}//ECHO_RIGHT(1);
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