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
#define MAX_VARIABLES 64

char prompt[256] = "hello";
char last_command[MAX_INPUT_SIZE] = "";
char history[MAX_HISTORY][MAX_INPUT_SIZE];
int history_count = 0;
int last_status = 0;
pid_t child_pid = -1; // Track child process ID

typedef struct {
    char name[256];
    char value[256];
} variable;

variable variables[MAX_VARIABLES];
int variable_count = 0;

void handle_sigint(int sig) {
    if (child_pid != -1) {
        kill(child_pid, SIGINT); // Send SIGINT to the child process
    } else {
        printf("\nYou typed Control-C!\n%s: ", prompt);
        fflush(stdout);
    }
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

char* get_variable_value(const char* name) {
    for (int i = 0; i < variable_count; ++i) {
        if (strcmp(variables[i].name, name) == 0) {
            return variables[i].value;
        }
    }
    return NULL;
}

void set_variable(const char* name, const char* value) {
    for (int i = 0; i < variable_count; ++i) {
        if (strcmp(variables[i].name, name) == 0) {
            strcpy(variables[i].value, value);
            return;
        }
    }
    if (variable_count < MAX_VARIABLES) {
        strcpy(variables[variable_count].name, name);
        strcpy(variables[variable_count].value, value);
        ++variable_count;
    } else {
        printf("Error: Maximum number of variables reached.\n");
    }
}

void setVariable(const char *command) {
    size_t pos = strchr(command, '=') - command;
    if (pos == strlen(command)) {
        printf("Syntax error: no '=' found.\n");
        return;
    }

    char varName[256];
    char varValue[256];

    strncpy(varName, command, pos);
    varName[pos] = '\0';

    strcpy(varValue, command + pos + 1);

    if (varName[0] != '$') {
        printf("Syntax error: must start with '$'.\n");
        return;
    }

    memmove(varName, varName + 1, strlen(varName));

    while (varName[0] != '\0' && varName[strlen(varName) - 1] == ' ') {
        varName[strlen(varName) - 1] = '\0';
    }
    while (varValue[0] == ' ') {
        memmove(varValue, varValue + 1, strlen(varValue));
    }

    set_variable(varName, varValue);
}

char *exec_command(const char *cmd) {
    char buffer[128];
    char *result = NULL;
    size_t result_size = 1;
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        perror("popen");
        return NULL;
    }
    
    result = malloc(result_size);
    if (!result) {
        perror("malloc");
        pclose(pipe);
        return NULL;
    }
    result[0] = '\0';

    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        size_t buffer_len = strlen(buffer);
        char *temp = realloc(result, result_size + buffer_len);
        if (!temp) {
            perror("realloc");
            free(result);
            pclose(pipe);
            return NULL;
        }
        result = temp;
        strcpy(result + result_size - 1, buffer);
        result_size += buffer_len;
    }

    pclose(pipe);
    return result;
}

void execute_command(char *command);
void execute_commands(char **commands, int num_commands) {
    for (int i = 0; i < num_commands; i++) {
        execute_command(commands[i]);
    }
}
void execute_command(char *command) {
    char *argv[MAX_TOKENS];
    char *cmd[MAX_TOKENS][MAX_TOKENS]; // To hold commands separated by pipes
    int i, j, k, fd, amper, redirect_out, redirect_err, redirect_in, append, retid, status, cmd_count;
    int fildes[2];
    char *outfile, *errfile, *infile;

    redirect_out = 0;
    redirect_err = 0;
    redirect_in = 0;
    append = 0;

    // Tokenize the command based on pipes
    i = 0;
    char *token = strtok(command, "|");
    while (token != NULL && i < MAX_TOKENS) {
        cmd[i++][0] = token;
        token = strtok(NULL, "|");
    }
    cmd_count = i;

    // Tokenize each command segment based on spaces
    for (i = 0; i < cmd_count; i++) {
        j = 0;
        token = strtok(cmd[i][0], " ");
        while (token != NULL && j < MAX_TOKENS) {
            cmd[i][j++] = token;
            token = strtok(NULL, " ");
        }
        cmd[i][j] = NULL;
    }

    // Check for redirections in the last command segment
    i = 0;
    while (cmd[cmd_count-1][i] != NULL) {
        if (strcmp(cmd[cmd_count-1][i], ">") == 0) {
            redirect_out = 1;
            append = 0;
            cmd[cmd_count-1][i] = NULL;
            outfile = cmd[cmd_count-1][i + 1];
            break;
        } else if (strcmp(cmd[cmd_count-1][i], ">>") == 0) {
            redirect_out = 1;
            append = 1;
            cmd[cmd_count-1][i] = NULL;
            outfile = cmd[cmd_count-1][i + 1];
            break;
        } else if (strcmp(cmd[cmd_count-1][i], "2>") == 0) {
            redirect_err = 1;
            cmd[cmd_count-1][i] = NULL;
            errfile = cmd[cmd_count-1][i + 1];
            break;
        } else if (strcmp(cmd[cmd_count-1][i], "<") == 0) {
            redirect_in = 1;
            cmd[cmd_count-1][i] = NULL;
            infile = cmd[cmd_count-1][i + 1];
            break;
        }
        i++;
    }

    // Check if the command ends with &
    if (strcmp(cmd[cmd_count-1][i-1], "&") == 0) {
        amper = 1;
        cmd[cmd_count-1][i-1] = NULL;
    } else {
        amper = 0;
    }

    // Execute the commands in the pipeline
    int in_fd = 0;
    for (i = 0; i < cmd_count; i++) {
        pipe(fildes);

        if ((child_pid = fork()) == 0) {
            if (i < cmd_count - 1) {
                dup2(fildes[1], STDOUT_FILENO);
            } else {
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
            }

            dup2(in_fd, STDIN_FILENO);
            close(fildes[0]);

            execvp(cmd[i][0], cmd[i]);
            perror("execvp");
            exit(EXIT_FAILURE);
        }

        close(fildes[1]);
        in_fd = fildes[0];
    }

    if (amper == 0) {
        for (i = 0; i < cmd_count; i++) {
            wait(&status);
            last_status = WEXITSTATUS(status);
        }
        child_pid = -1;
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
        if (strlen(input) == 0) {
            continue;
        }

        if (strcmp(input, "Control-C") == 0) {
            printf("You typed Control-C!\n");
            continue;
        }

        // Check for variable assignment
        if (input[0] == '$' && strchr(input, '=') != NULL) {
            setVariable(input);
            continue;
        }

        // Check for variable usage
        if (strncmp(input, "echo $", 6) == 0) {
            char *var_name = input + 6;
            char *value = get_variable_value(var_name);
            if (value != NULL) {
                printf("%s\n", value);
            } else {
                printf("%s\n", var_name);
            }
            continue;
        }

        add_to_history(input);

        // Built-in commands
        if (strncmp(input, "cd ", 3) == 0) {
            char *dir = input + 3;
            if (chdir(dir) != 0) {
                perror("cd");
            }
        } else if (strncmp(input, "echo ", 5) == 0) {
            if (strcmp(input + 5, "$?") == 0) {
                printf("%d\n", last_status);
            } else {
                printf("%s\n", input + 5);
            }
        } else if (strncmp(input, "prompt = ", 9) == 0) {
            strcpy(prompt, input + 9);
        } else if (strcmp(input, "!!") == 0) {
            if (strlen(last_command) == 0) {
                printf("No previous command.\n");
            } else {
                printf("%s\n", last_command);
                execute_command(last_command);
                add_to_history(last_command); // Add the executed command to history
            }
        } else if (strcmp(input, "quit") == 0) {
            break; // Exit the shell
        } else if (strncmp(input, "if ", 3) == 0) {
            char *if_statement = input + 3; // Get the condition statement
            char temp_command[MAX_INPUT_SIZE];
            char *then_commands[MAX_INPUT_SIZE];
            char *else_commands[MAX_INPUT_SIZE];
            int then_count = 0;
            int else_count = 0;

            printf("> ");
            if (fgets(temp_command, sizeof(temp_command), stdin) == NULL) {
                break;
            }
            temp_command[strcspn(temp_command, "\n")] = 0; // Remove trailing newline
            if (strncmp(temp_command, "then", 4) != 0) {
                printf("Error: Expected then after if statement.\n");
                continue;
            }

            // Read "then" block commands
            while (1) {
                printf("> ");
                if (fgets(temp_command, sizeof(temp_command), stdin) == NULL) {
                    break;
                }
                temp_command[strcspn(temp_command, "\n")] = 0; // Remove trailing newline
                if (strcmp(temp_command, "else") == 0) {
                    break;
                }
                then_commands[then_count] = strdup(temp_command);
                then_count++;
            }

            // Read "else" block commands
            while (1) {
                printf("> ");
                if (fgets(temp_command, sizeof(temp_command), stdin) == NULL) {
                    break;
                }
                temp_command[strcspn(temp_command, "\n")] = 0; // Remove trailing newline
                if (strcmp(temp_command, "fi") == 0) {
                    break;
                }
                else_commands[else_count] = strdup(temp_command);
                else_count++;
            }

            // Execute the appropriate command list
            if (system(if_statement) == 0) {
                execute_commands(then_commands, then_count);
            } else {
                execute_commands(else_commands, else_count);
            }

            // Free allocated memory
            for (int i = 0; i < then_count; i++) {
                free(then_commands[i]);
            }
            for (int i = 0; i < else_count; i++) {
                free(else_commands[i]);
            }
        } else {
            strcpy(last_command, input);
            execute_command(input);
        }
    }

    return 0;
}
