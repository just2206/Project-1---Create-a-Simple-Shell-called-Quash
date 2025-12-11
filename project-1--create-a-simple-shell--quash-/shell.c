#define _POSIX_C_SOURCE 200809L // Or any value >= 199309L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>    
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>    
#include <errno.h>
#include <sys/types.h>        

#define MAX_COMMAND_LINE_LEN 1024
#define MAX_COMMAND_LINE_ARGS 128
#define MAX_PATH_LEN 1024

char prompt[] = "> ";
char delimiters[] = " \t\r\n";
extern char **environ; 

// Global variable for foreground PID, used by SIGALRM handler
pid_t foreground_pid = 0;

// Function Prototypes
int tokenize(char *command_line, char *arguments[]);
void print_prompt();
int execute_builtin(char *arguments[], int arg_count);
void alarm_handler(int signum);
int find_pipe(char *arguments[]);
void execute_pipe(char *cmd1_args[], char *cmd2_args[], bool is_background);
void execute_single_command(char *arguments[], int arg_count, bool is_background);

// --- Signal Handler for SIGALRM (Timer Expiration) ---
void alarm_handler(int signum) {
    // Only kill if a foreground process is registered
    if (foreground_pid > 0) {
        fprintf(stderr, "\nProcess %d timed out after 10 seconds. Terminating...\n", foreground_pid);
        
        // Use SIGKILL (9) to forcefully terminate the child
        if (kill(foreground_pid, SIGKILL) == -1) {
            // Ignore if the child has already terminated
            if (errno != ESRCH) { 
                perror("kill failed in alarm_handler");
            }
        }
    }
    signal(SIGALRM, alarm_handler);
}

// --- Tokenization and Variable Expansion ---
int tokenize(char *command_line, char *arguments[]) {
    int i = 0;
    char *token = strtok(command_line, delimiters);

    while (token != NULL && i < MAX_COMMAND_LINE_ARGS - 1) {
        if (token[0] == '$') {
            char *var_name = token + 1;
            char *var_value = getenv(var_name);

            if (var_value != NULL) {
                // Replace $VAR with its value
                arguments[i++] = var_value;
            } else {
                fprintf(stderr, "shell: warning: environment variable '%s' not set\n", var_name);
            }
        } else {
            arguments[i++] = token;
        }

        token = strtok(NULL, delimiters);
    }
    arguments[i] = NULL;
    return i;
}

// --- Prompt and Built-in Commands (Executed in Parent) ---
void print_prompt() {
    char cwd[MAX_PATH_LEN];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s%s", cwd, prompt);
    } else {
        perror("getcwd error");
        printf("%s", prompt);
    }
    fflush(stdout);
}

int execute_builtin(char *arguments[], int arg_count) {
    if (arguments[0] == NULL) return 0;

    if (strcmp(arguments[0], "exit") == 0) {
        exit(0);
    } else if (strcmp(arguments[0], "pwd") == 0) {
        char cwd[MAX_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
        } else {
            perror("pwd error");
        }
        return 1;
    } else if (strcmp(arguments[0], "cd") == 0) {
        char *target_dir = (arg_count > 1) ? arguments[1] : getenv("HOME");
        if (target_dir == NULL) {
            fprintf(stderr, "cd: HOME not set\n");
            return 1;
        }
        if (chdir(target_dir) != 0) {
            perror("cd error");
        }
        return 1;
    } else if (strcmp(arguments[0], "echo") == 0) {
        for (int i = 1; i < arg_count; i++) {
            printf("%s%s", arguments[i], (i == arg_count - 1) ? "" : " ");
        }
        printf("\n");
        return 1;
    } else if (strcmp(arguments[0], "env") == 0) {
        for (char **env = environ; *env != NULL; env++) {
            printf("%s\n", *env);
        }
        return 1;
    } else if (strcmp(arguments[0], "setenv") == 0) {
        if (arg_count != 3) {
            fprintf(stderr, "Usage: setenv <VARIABLE> <VALUE>\n");
        } else {
            if (setenv(arguments[1], arguments[2], 1) != 0) {
                perror("setenv error");
            }
        }
        return 1;
    }
    return 0; // Not a built-in command
}

// --- Process Execution Functions (Uses fork/execvp) ---
int find_pipe(char *arguments[]) {
    for (int i = 0; arguments[i] != NULL; i++) {
        if (strcmp(arguments[i], "|") == 0) {
            return i;
        }
    }
    return -1; 
}

void execute_pipe(char *cmd1_args[], char *cmd2_args[], bool is_background) {
    int pipefd[2];
    pid_t pid1, pid2;
    int status;
    
    // Pipe logic is complex to integrate with the timer, so piping commands
    // are executed as standard foreground tasks without the 10-second timer.

    if (pipe(pipefd) == -1) {
        perror("pipe failed");
        return;
    }

    // Fork 1: Command 1 (Writer)
    pid1 = fork();
    if (pid1 < 0) { perror("fork cmd1 failed"); return; }
    if (pid1 == 0) {
        close(pipefd[0]); // Close read end
        dup2(pipefd[1], STDOUT_FILENO); // Redirect stdout to pipe
        close(pipefd[1]); 

        signal(SIGINT, SIG_DFL); // Restore signals
        alarm(0);
        signal(SIGALRM, SIG_DFL);

        execvp(cmd1_args[0], cmd1_args);
        perror(cmd1_args[0]);
        exit(1);
    }

    // Fork 2: Command 2 (Reader)
    pid2 = fork();
    if (pid2 < 0) { perror("fork cmd2 failed"); return; }
    if (pid2 == 0) {
        close(pipefd[1]); // Close write end
        dup2(pipefd[0], STDIN_FILENO); // Redirect stdin from pipe
        close(pipefd[0]); 

        signal(SIGINT, SIG_DFL); // Restore signals
        alarm(0);
        signal(SIGALRM, SIG_DFL);
        
        execvp(cmd2_args[0], cmd2_args);
        perror(cmd2_args[0]);
        exit(1);
    }

    // Parent: Close pipe ends
    close(pipefd[0]);
    close(pipefd[1]);

    // Parent waits for both children
    waitpid(pid1, &status, 0);
    waitpid(pid2, &status, 0);
}

void execute_single_command(char *arguments[], int arg_count, bool is_background) {
    // 3. Create a child process which will execute the command
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork failed");
    } 
    // Child Process
    else if (pid == 0) {
        // Essential for child: clear parent's timer and handler before exec
        alarm(0);
        signal(SIGALRM, SIG_DFL);
        
        // Restore default SIGINT handling for foreground processes
        if (!is_background) {
            signal(SIGINT, SIG_DFL);
        }
        
        // Execute the command, replacing the child's process image
        execvp(arguments[0], arguments);
        
        // If execvp returns, it failed
        perror(arguments[0]);
        exit(errno == ENOENT ? 127 : 1); 
    } 
    // Parent Process
    else {
        if (is_background) {
            printf("[Background job] PID: %d\n", pid);
            // Non-blocking wait to clean up finished background jobs
            while (waitpid(-1, NULL, WNOHANG) > 0);
            
        } else {
            // Foreground process: Set timer and wait
            foreground_pid = pid; 
            
            // Start the 10-second timer
            alarm(10); 

            int status;
            // Wait for the child process. It can be interrupted by SIGALRM or SIGINT.
            if (waitpid(pid, &status, 0) == -1) {
                if (errno == EINTR) { 
                    // Wait again to reap the child if it was just killed by SIGALRM/SIGKILL
                    waitpid(pid, &status, WNOHANG); 
                } else {
                    perror("waitpid error");
                }
            }
            
            // Stop the timer immediately
            alarm(0); 
            foreground_pid = 0; 
            
            // Handle cleanup message if foreground child was killed by Ctrl-C
            if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) {
                printf("\n"); 
            }
        }
    }
}

// --- Main Shell Loop ---
int main() {
    char command_line[MAX_COMMAND_LINE_LEN];
    char *arguments[MAX_COMMAND_LINE_ARGS];
    int arg_count;
    
    // Set up signal handlers
    signal(SIGINT, SIG_IGN); // Shell ignores Ctrl-C
    signal(SIGALRM, alarm_handler); // Shell handles timer
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN; 
    sa.sa_flags = SA_NOCLDWAIT; 
    sigaction(SIGCHLD, &sa, NULL); // Clean up zombies

    while (true) {
        foreground_pid = 0;
        
        do { 
            print_prompt();
            if ((fgets(command_line, MAX_COMMAND_LINE_LEN, stdin) == NULL)) {
                if (feof(stdin)) {
                    printf("\nexit\n");
                    return 0;
                }
                if (ferror(stdin) && errno == EINTR) {
                    clearerr(stdin);
                    break; 
                } else if (ferror(stdin)) {
                    fprintf(stderr, "fgets error");
                    exit(1);
                }
            }
        } while(command_line[0] == 0x0A);
        
        if (ferror(stdin) && errno == EINTR) continue;
        if (feof(stdin) || command_line[0] == '\0') continue; 
        
        command_line[strcspn(command_line, "\n")] = 0;

        char cmd_copy[MAX_COMMAND_LINE_LEN];
        strcpy(cmd_copy, command_line);
        arg_count = tokenize(cmd_copy, arguments);
        
        if (arg_count == 0) continue;

        // Check and remove background symbol
        bool is_background = false;
        if (arg_count > 0 && strcmp(arguments[arg_count - 1], "&") == 0) {
            is_background = true;
            arguments[arg_count - 1] = NULL;
            arg_count--;
        }

        // 1. Check for built-in command (executed directly in parent)
        if (execute_builtin(arguments, arg_count)) {
            continue;
        }

        // 2. Check for pipe operator '|'
        int pipe_index = find_pipe(arguments);
        
        if (pipe_index > 0) {
            char *cmd1_args[MAX_COMMAND_LINE_ARGS];
            memcpy(cmd1_args, arguments, pipe_index * sizeof(char *));
            cmd1_args[pipe_index] = NULL;
            
            char *cmd2_args[MAX_COMMAND_LINE_ARGS];
            int j = 0;
            for (int i = pipe_index + 1; arguments[i] != NULL; i++) {
                cmd2_args[j++] = arguments[i];
            }
            cmd2_args[j] = NULL;

            if (cmd1_args[0] == NULL || cmd2_args[0] == NULL) {
                fprintf(stderr, "Shell: Invalid pipe command format.\n");
            } else {
                execute_pipe(cmd1_args, cmd2_args, is_background);
            }
            
        } else {
            // 3. Execute a single external command (uses fork/execvp)
            execute_single_command(arguments, arg_count, is_background);
        }
    }
    return -1;
}