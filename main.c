#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <sys/types.h>

#define ASSERT_PERROR(Condition, Error) do { if (!(Condition)) { perror(Error); goto out; } } while (0)

#define CTRL_C 3
#define CTRL_D 4

/////////////
// Interactive Shell
/////////////

static char *prompt() {
    char *ps1 = getenv("PS1");
    if (ps1)
        return ps1;
    if (!getuid())
        return "# ";
    return "$ ";
}

struct shellinput {
    char   shin_buf[0x1000];
    char  *shin_ptr;
    size_t shin_cnt;
};

void shellinput_clear(struct shellinput *shin) {
    shin->shin_cnt = 0;
    if (shin->shin_ptr) {
        free(shin->shin_ptr);
        shin->shin_ptr = NULL;
    }
}

int shellinput_add(struct shellinput *shin, char c) {
    if (shin->shin_cnt < sizeof(shin->shin_buf)) {
        shin->shin_buf[shin->shin_cnt++] = c;
        return 0;
    }

    if (shin->shin_cnt == sizeof(shin->shin_buf)) {
        if (NULL == (shin->shin_ptr = malloc(sizeof(shin->shin_buf) * 2)))
            return 1;
        memmove(shin->shin_ptr, shin->shin_buf, sizeof(shin->shin_buf));
    }
    else if (shin->shin_cnt % sizeof(shin->shin_buf) == 0) {
        if (NULL == (shin->shin_ptr = realloc(shin->shin_ptr, shin->shin_cnt + sizeof(shin->shin_buf))))
            return 1;
    }

    shin->shin_ptr[shin->shin_cnt++] = c;
    return 0;
}

#define ECHO_CNTRL(C) printf("^%c", 'A'+C-1)

#define PRMT_EXIT ((void *)0)
#define PRMT_ABRT ((void *)-1)

/**
 * 
 */
static char *prompt(struct termios *termios_p)
{
    char *ret = PRMT_ABRT;
    struct termios raw_termios;
    struct shellinput shin = {0};

    // set terminal to raw mode
    memcpy(&raw_termios, termios_p, sizeof(raw_termios));
    raw_termios.c_iflag &= ~(IXON);
    raw_termios.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    ASSERT_PERROR(tcsetattr(STDIN_FILENO, TCSADRAIN, &raw_termios) == 0, "tcsetattr");

    fputs(prompt(), stdout);

    while (1)
    {
        int c;
        ASSERT_PERROR(EOF != (c = getchar()), "getchar");
        
        if (c == CTRL_D) {
            ret = PRMT_EXIT;
            putchar('\n');
            goto out;
        }

        if (c == '\n')
            break;
        
        if (c == CTRL_C) {
            shellinput_clear(&shin);
            ECHO_CNTRL(c);
            putchar('\n');
            continue;
        }
        
        if (!iscntrl(c)) {
            putchar(c);
            shellinput_add(&shin, c);
        }
    }
    putchar('\n');

out:
    tcsetattr(STDIN_FILENO, TCSADRAIN, termios_p);
    return ret;
}

static int interactive() {
    int ret = 1;

    struct termios saved_termios;
    struct termios termios;
    int got_termios = 0;

    pid_t shpgid;
    
    int c;

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
    memcpy(&saved_termios, &termios, sizeof(saved_termios));
    got_termios = 1;

    // set terminal to raw mode
    termios.c_iflag &= ~(IXON);
    termios.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    ASSERT_PERROR(tcsetattr(STDIN_FILENO, TCSADRAIN, &termios) == 0, "tcsetattr");

    while (1) {
        int discard = 0;
        fputs(prompt(), stdout);

        // read line
        shellinput_clear(&shin);
        while (!discard) {
            ASSERT_PERROR(EOF != (c = getchar()), "getchar");
            
            if (c == CTRL_D) {
                ret = 0;
                putchar('\n');
                goto out;
            }

            if (c == '\n')
                break;
            
            if (c == CTRL_C) {
                discard = 1;
                ECHO_CNTRL(c);
                break;
            }
            

            if (!iscntrl(c)) {
                putchar(c);
                shellinput_add(&shin, c);
            }
        }
        putchar('\n');
    }

    ret = 0;
out:
    if (got_termios)
        tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_termios);
    return ret;
}

int main(int argc, char **argv)
{
    if (isatty(STDIN_FILENO))
        return interactive(STDIN_FILENO);
    
    fprintf(stderr, "interactive only\n");
    return 1;
}