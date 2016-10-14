#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <pthread.h>
#include <signal.h>
#include <binn.h>
#include <stdbool.h>

#include "psp2cmd.h"
#include "cmd.h"
#include "utility.h"
#include "main.h"
#include "errno.h"

enum colors_t {
    COL_NONE = 0,
    COL_RED = 1,
    COL_YELLOW = 2,
    COL_GREEN = 3
};

int msg_sock = -1;
int data_sock = -1;
int done = 0;
char **_argv;

void *msg_thread(void *unused);

// readline
char history_path[512];

void process_line(char *line);

tcflag_t old_lflag;
cc_t old_vtime;
struct termios term;
int readline_callback = 0;
// readline

void setup_terminal() {
    if (tcgetattr(STDIN_FILENO, &term) < 0) {
        perror("tcgetattr");
        exit(1);
    }
    old_lflag = term.c_lflag;
    old_vtime = term.c_cc[VTIME];
    term.c_lflag &= ~ICANON;
    term.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &term) < 0) {
        perror("tcsetattr");
        exit(1);
    }
    rl_callback_handler_install("psp2shell> ", process_line);
    readline_callback = 1;
}

void close_terminal() {
    term.c_lflag = old_lflag;
    term.c_cc[VTIME] = old_vtime;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &term) < 0) {
        perror("tcsetattr");
        exit(1);
    }
    readline_callback = 0;
    rl_callback_handler_remove();
    fflush(stdin);
}

void reset_terminal(void) {
    rl_replace_line("", 0);
    rl_refresh_line(0, 0);
}

void sig_handler(int sig) {
    if (sig == SIGINT) {
        reset_terminal();
    }
}

int get_sock(int sock, char *ip, int port, bool verbose) {

    struct sockaddr_in addr;
    bzero((char *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons((uint16_t) port);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("%s\n", strerror(errno));
        return -1;
    }

    if (verbose) {
        printf("connecting to %s:%d ... ", ip, port);
        fflush(stdout);
    }

    while (connect(sock, (struct sockaddr *) &addr, sizeof(struct sockaddr)) < 0) {
        if (errno != EINPROGRESS && errno != EALREADY) {
            if (verbose) {
                printf("%s\n", strerror(errno));
            }
        } else {
            if (verbose) {
                printf("\n");
            }
        }
        if (verbose) {
            printf("connecting to %s:%d ... ", ip, port);
            fflush(stdout);
        }
        sleep(2);
    }

    return sock;
}

int connect_psp2(char *address, int port) {

    if ((msg_sock = get_sock(msg_sock, address, port, true)) < 0) {
        printf("get_sock failed (port=%i)\n", port);
        return msg_sock;
    }

    pthread_t resp_thread;
    if (pthread_create(&resp_thread, NULL, msg_thread, NULL) < 0) {
        printf("could not create thread\n");
        exit(EXIT_FAILURE);
    }

    if ((data_sock = get_sock(data_sock, address, port + 1, false)) < 0) {
        printf("get_sock failed (port=%i)\n", port + 1);
        close(msg_sock);
        return data_sock;
    }

    setup_terminal();
    // catch CTRL+C
    signal(SIGINT, sig_handler);

    return 0;
}

void *msg_thread(void *unused) {

    char *msg = malloc(SIZE_CMD);
    memset(msg, 0, SIZE_CMD);

    // receive message from psp2shell
    while ((recv(msg_sock, msg, SIZE_CMD, 0)) > 0) {

        int type, count, size;
        BOOL is_msg = binn_is_valid(binn_ptr(msg), &type, &count, &size);
        if (is_msg) {
            char *str = binn_object_str(msg, "0");
            switch (binn_object_int32(msg, "c")) {
                case COL_RED:
                    printf(RED "%s" RES, str);
                    break;
                case COL_YELLOW:
                    printf(YEL "%s" RES, str);
                    break;
                case COL_GREEN:
                    printf(GRN "%s" RES, str);
                    break;
                default:
                    printf("%s", str);
                    break;
            }
            fflush(stdout);
        }

        memset(msg, 0, SIZE_CMD);

        rl_newline(0, 0);
        rl_refresh_line(0, 0);

        // send "ok/continue"
        send(msg_sock, "\n", 1, 0);
    }

    printf("disconnected\n");
    signal(SIGINT, SIG_DFL);
    close_terminal();
    free(msg);
    close(msg_sock);
    close(data_sock);
    msg_sock = -1;
    data_sock = -1;

    // restart
    execvp(_argv[0], _argv);
}

void process_line(char *line) {

    if (line == NULL) {
        close_terminal();
        exit(0);
    }

    if (strlen(line) > 0) {

        // handle history
        add_history(line);
        append_history(1, history_path);

        // parse cmd
        size_t num_tokens;
        char **tokens = strsplit(line, " ", &num_tokens);
        if (num_tokens > 0) {
            COMMAND *cmd = cmd_find(tokens[0]);
            if (cmd == NULL) {
                printf("Command not found. Use ? for help.\n");
            } else {
                printf("\n");
                cmd->func((int) num_tokens, tokens);
                printf("\n");
                for (int i = 0; i < num_tokens; i++) {
                    if (tokens[i] != NULL)
                        free(tokens[i]);
                }
            }
        }
        if (tokens != NULL) {
            free(tokens);
        }
    }

    free(line);
}

int process_args(int argc, char **argv) {

    char *line = malloc(1024);

    connect_psp2(argv[1], atoi(argv[2]));

    memset(line, 0, 1024);
    for (int i = 3; i < argc; i++) {
        sprintf(line + strlen(line), "%s ", argv[i]);
    }
    process_line(line);
    close_terminal();

    return 0;
}

int main(int argc, char **argv) {

    fd_set fds;

    if (argc < 3) {
        fprintf(stderr, "Usage %s <address> <port_num> (<cmd>)\n", argv[0]);
        return EXIT_FAILURE;
    } else if (argc > 3) {
        process_args(argc, argv);
        exit(0);
    }

    _argv = argv;

    // load history from file
    memset(history_path, 0, 512);
    snprintf(history_path, 512, "%s/.psp2shell_history", getenv("HOME"));
    if (read_history(history_path) != 0) { // append_history needs the file created
        write_history(history_path);
    }

    while (!done) {

        if (msg_sock < 0) {
            if (connect_psp2(argv[1], atoi(argv[2])) < 0) {
                break;
            }
        } else {
            FD_ZERO(&fds);
            FD_SET(fileno(stdin), &fds);
            if (select(fileno(stdin) + 1, &fds, NULL, NULL, NULL) < 0) {
                //continue (CTRL+C)
            } else if (FD_ISSET(fileno(stdin), &fds) && readline_callback) {
                rl_callback_read_char();
            }
        }
    }

    close_terminal();
    return -1;
}
