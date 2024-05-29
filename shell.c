#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#define MAX_INPUT_SIZE 1024
#define MAX_TOKENS 64
#define MAX_HISTORY 20

char prompt[256] = "hello";
char last_command[MAX_INPUT_SIZE] = "";
char history[MAX_HISTORY][MAX_INPUT_SIZE];
int history_count = 0;
int last_status = 0;

void handle_sigint(int sig) {
    printf("\nYou typed Control-C!\n%s: ", prompt);
    fflush(stdout);
}

void add_to_history(char *command) {
    if (history_count < MAX_HISTORY) {
        strcpy(history[history_count++], command);
    } else {
        for (int i = 1; i < MAX_HISTORY; i++) {
            strcpy(history[i - 1], history[i]);
        }
        strcpy(history[MAX_HISTORY - 1], command);
    }
}

void execute_command(char *command);

void execute_conditional(char **args) {
    int status;
    pid_t pid;
    
    if ((pid = fork()) == 0) {
        execvp(args[0], args);
        perror("execvp");
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        waitpid(pid, &status, 0);
        last_status = WEXITSTATUS(status);
    } else {
        perror("fork");
        exit(EXIT_FAILURE);
    }
}

void handle_if_else(char *command) {
    char *token = strtok(command, " ");
    if (strcmp(token, "if") != 0) {
        fprintf(stderr, "Syntax error: expected 'if'\n");
        return;
    }

    char *condition[MAX_TOKENS];
    int i = 0;
    while ((token = strtok(NULL, " ")) != NULL && strcmp(token, "then") != 0) {
        condition[i++] = token;
    }
    condition[i] = NULL;

    if (token == NULL) {
        fprintf(stderr, "Syntax error: expected 'then'\n");
        return;
    }

    char then_command[MAX_INPUT_SIZE] = "";
    char else_command[MAX_INPUT_SIZE] = "";
    int else_found = 0;

    while ((token = strtok(NULL, " ")) != NULL) {
        if (strcmp(token, "else") == 0) {
            else_found = 1;
            break;
        }
        strcat(then_command, token);
        strcat(then_command, " ");
    }

    while ((token = strtok(NULL, " ")) != NULL && strcmp(token, "fi") != 0) {
        strcat(else_command, token);
        strcat(else_command, " ");
    }

    if (token == NULL) {
        fprintf(stderr, "Syntax error: expected 'fi'\n");
        return;
    }

    execute_conditional(condition);

    if (last_status == 0) {
        execute_command(then_command);
    } else if (else_found) {
        execute_command(else_command);
    }
}

void execute_command(char *command) {
    if (strncmp(command, "if ", 3) == 0) {
        handle_if_else(command);
        return;
    }

    char *argv1[MAX_TOKENS];
    char *argv2[MAX_TOKENS];
    char *token;
    int i, fd, amper, redirect_out, redirect_err, redirect_in, append, piping, retid, status, argc1;
    int fildes[2];
    char *outfile, *errfile, *infile;

    piping = 0;
    redirect_out = 0;
    redirect_err = 0;
    redirect_in = 0;
    append = 0;

    /* Parse command line */
    i = 0;
    token = strtok(command, " ");
    while (token != NULL) {
        argv1[i++] = token;
        token = strtok(NULL, " ");
        if (token && (strcmp(token, "|") == 0)) {
            piping = 1;
            break;
        }
    }
    argv1[i] = NULL;
    argc1 = i;

    /* Is command empty */
    if (argv1[0] == NULL) {
        return;
    }

    /* Does command contain pipe */
    if (piping) {
        i = 0;
        while ((token = strtok(NULL, " ")) != NULL) {
            argv2[i++] = token;
        }
        argv2[i] = NULL;
    }

    /* Check for redirections */
    for (i = 0; argv1[i] != NULL; i++) {
        if (strcmp(argv1[i], ">") == 0) {
            redirect_out = 1;
            append = 0;
            argv1[i] = NULL;
            outfile = argv1[i + 1];
            break;
        } else if (strcmp(argv1[i], ">>") == 0) {
            redirect_out = 1;
            append = 1;
            argv1[i] = NULL;
            outfile = argv1[i + 1];
            break;
        } else if (strcmp(argv1[i], "2>") == 0) {
            redirect_err = 1;
            argv1[i] = NULL;
            errfile = argv1[i + 1];
            break;
        } else if (strcmp(argv1[i], "<") == 0) {
            redirect_in = 1;
            argv1[i] = NULL;
            infile = argv1[i + 1];
            break;
        }
    }

    /* Does command line end with & */
    if (argc1 > 0 && strcmp(argv1[argc1 - 1], "&") == 0) {
        amper = 1;
        argv1[argc1 - 1] = NULL;
    } else {
        amper = 0;
    }

    /* For commands not part of the shell command language */
    if (fork() == 0) {
        /* Redirection of IO */
        if (redirect_out) {
            if (append) {
                fd = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0660);
            } else {
                fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0660);
            }
            if (fd < 0) {
                perror("open");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        if (redirect_err) {
            fd = open(errfile, O_WRONLY | O_CREAT | O_TRUNC, 0660);
            if (fd < 0) {
                perror("open");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (redirect_in) {
            fd = open(infile, O_RDONLY);
            if (fd < 0) {
                perror("open");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        if (piping) {
            pipe(fildes);
            if (fork() == 0) {
                /* First component of command line */
                close(STDOUT_FILENO);
                dup(fildes[1]);
                close(fildes[1]);
                close(fildes[0]);
                execvp(argv1[0], argv1);
                perror("execvp");
                exit(EXIT_FAILURE);
            }
            /* Second component of command line */
            close(STDIN_FILENO);
            dup(fildes[0]);
            close(fildes[0]);
            close(fildes[1]);
            execvp(argv2[0], argv2);
            perror("execvp");
            exit(EXIT_FAILURE);
        } else {
            execvp(argv1[0], argv1);
            perror("execvp");
            exit(EXIT_FAILURE);
        }
    }

    /* Parent continues over here... */
    /* Waits for child to exit if required */
    if (amper == 0) {
        retid = wait(&status);
        last_status = WEXITSTATUS(status);
    }
}

int main() {
    signal(SIGINT, handle_sigint);
    char input[MAX_INPUT_SIZE];

    while (1) {
        printf("%s: ", prompt);
        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\n");
            break; // Handle Ctrl+D to exit the shell
        }

        input[strcspn(input, "\n")] = 0; // Remove trailing newline
        if (strcmp(input, "exit") == 0) {
            break;
        }

        if (strlen(input) == 0) {
            continue;
        }

        add_to_history(input);
        execute_command(input);
    }

    return 0;
}
