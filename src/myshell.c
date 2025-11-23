#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define INITIAL_CAPACITY 8

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

typedef struct {
    char **argv;
    size_t argc;
    size_t capacity;
    char *input_redirect;
    char *output_redirect;
    bool append;
} Command;

typedef struct {
    Command *commands;
    size_t count;
    size_t capacity;
    bool background;
} Pipeline;

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} JobState;

typedef struct {
    int id;
    pid_t pgid;
    char *command;
    JobState state;
} Job;

static StringList history = {0};
static Job *jobs = NULL;
static size_t job_count = 0;
static size_t job_capacity = 0;
static int next_job_id = 1;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void *xrealloc(void *ptr, size_t bytes) {
    void *tmp = realloc(ptr, bytes);
    if (!tmp) {
        die("realloc");
    }
    return tmp;
}

static char *xstrdup(const char *src) {
    char *dup = strdup(src);
    if (!dup) {
        die("strdup");
    }
    return dup;
}

static void string_list_append(StringList *list, const char *value) {
    if (list->capacity == 0) {
        list->capacity = INITIAL_CAPACITY;
        list->items = xrealloc(NULL, list->capacity * sizeof(char *));
    } else if (list->count == list->capacity) {
        list->capacity *= 2;
        list->items = xrealloc(list->items, list->capacity * sizeof(char *));
    }
    list->items[list->count++] = xstrdup(value);
}

static void add_history_entry(const char *line) {
    if (!line || !*line) {
        return;
    }
    string_list_append(&history, line);
}

static void free_command(Command *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->argc; ++i) {
        free(cmd->argv[i]);
    }
    free(cmd->argv);
    free(cmd->input_redirect);
    free(cmd->output_redirect);
}

static void free_pipeline(Pipeline *pipeline) {
    if (!pipeline) {
        return;
    }
    for (size_t i = 0; i < pipeline->count; ++i) {
        free_command(&pipeline->commands[i]);
    }
    free(pipeline->commands);
}

static void command_add_arg(Command *cmd, const char *value) {
    if (cmd->capacity == 0) {
        cmd->capacity = INITIAL_CAPACITY;
        cmd->argv = xrealloc(NULL, cmd->capacity * sizeof(char *));
    } else if (cmd->argc + 1 >= cmd->capacity) {
        cmd->capacity *= 2;
        cmd->argv = xrealloc(cmd->argv, cmd->capacity * sizeof(char *));
    }
    cmd->argv[cmd->argc++] = xstrdup(value);
    cmd->argv[cmd->argc] = NULL;
}

static void pipeline_add_command(Pipeline *pipeline, Command *cmd) {
    if (pipeline->capacity == 0) {
        pipeline->capacity = INITIAL_CAPACITY;
        pipeline->commands = xrealloc(NULL, pipeline->capacity * sizeof(Command));
    } else if (pipeline->count == pipeline->capacity) {
        pipeline->capacity *= 2;
        pipeline->commands = xrealloc(pipeline->commands, pipeline->capacity * sizeof(Command));
    }
    pipeline->commands[pipeline->count++] = *cmd;
}

static void skip_whitespace(const char **cursor) {
    while (**cursor && isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

static char *consume_token(const char **cursor) {
    const char *start = *cursor;
    char quote = 0;
    bool escape = false;
    size_t capacity = 32;
    size_t length = 0;
    char *buffer = xrealloc(NULL, capacity);

    while (**cursor) {
        char ch = **cursor;
        if (escape) {
            buffer[length++] = ch;
            escape = false;
        } else if (ch == '\\') {
            escape = true;
        } else if (quote) {
            if (ch == quote) {
                quote = 0;
            } else {
                buffer[length++] = ch;
            }
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (isspace((unsigned char)ch) || ch == '|' || ch == '<' || ch == '>' || ch == '&') {
            break;
        } else {
            buffer[length++] = ch;
        }
        if (length + 1 >= capacity) {
            capacity *= 2;
            buffer = xrealloc(buffer, capacity);
        }
        (*cursor)++;
    }

    if (quote) {
        fprintf(stderr, "Unterminated quote detected\n");
        free(buffer);
        return NULL;
    }

    buffer[length] = '\0';
    if (length == 0) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

static char **tokenize(const char *line, size_t *out_count) {
    size_t capacity = INITIAL_CAPACITY;
    size_t count = 0;
    char **tokens = xrealloc(NULL, capacity * sizeof(char *));
    const char *cursor = line;

    while (*cursor) {
        skip_whitespace(&cursor);
        if (!*cursor) {
            break;
        }

        if (count == capacity) {
            capacity *= 2;
            tokens = xrealloc(tokens, capacity * sizeof(char *));
        }

        if (*cursor == '|') {
            tokens[count++] = xstrdup("|");
            cursor++;
            continue;
        }
        if (*cursor == '&') {
            tokens[count++] = xstrdup("&");
            cursor++;
            continue;
        }
        if (*cursor == '>') {
            cursor++;
            if (*cursor == '>') {
                tokens[count++] = xstrdup(">>");
                cursor++;
            } else {
                tokens[count++] = xstrdup(">");
            }
            continue;
        }
        if (*cursor == '<') {
            tokens[count++] = xstrdup("<");
            cursor++;
            continue;
        }

        char *token = consume_token(&cursor);
        if (!token) {
            for (size_t i = 0; i < count; ++i) {
                free(tokens[i]);
            }
            free(tokens);
            return NULL;
        }
        tokens[count++] = token;
    }

    *out_count = count;
    return tokens;
}

static void free_tokens(char **tokens, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        free(tokens[i]);
    }
    free(tokens);
}

static bool parse_pipeline(char **tokens, size_t token_count, Pipeline *pipeline) {
    memset(pipeline, 0, sizeof(*pipeline));
    Command current = {0};

    for (size_t i = 0; i < token_count; ++i) {
        char *tok = tokens[i];

        if (strcmp(tok, "|") == 0) {
            if (current.argc == 0 && !current.input_redirect && !current.output_redirect) {
                fprintf(stderr, "Syntax error near unexpected token '|'\n");
                free_command(&current);
                free_pipeline(pipeline);
                return false;
            }
            pipeline_add_command(pipeline, &current);
            memset(&current, 0, sizeof(current));
            continue;
        }

        if (strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
            bool append = (strcmp(tok, ">>") == 0);
            bool input = (strcmp(tok, "<") == 0);
            if (i + 1 >= token_count) {
                fprintf(stderr, "Redirection without target file\n");
                free_command(&current);
                free_pipeline(pipeline);
                return false;
            }
            char *path = xstrdup(tokens[++i]);
            if (input) {
                free(current.input_redirect);
                current.input_redirect = path;
            } else {
                free(current.output_redirect);
                current.output_redirect = path;
                current.append = append;
            }
            continue;
        }

        if (strcmp(tok, "&") == 0) {
            if (i != token_count - 1) {
                fprintf(stderr, "Background symbol '&' must be at end of command\n");
                free_command(&current);
                free_pipeline(pipeline);
                return false;
            }
            pipeline->background = true;
            continue;
        }

        command_add_arg(&current, tok);
    }

    if (current.argc == 0 && pipeline->count == 0) {
        free_command(&current);
        return false;
    }

    if (current.argc > 0 || current.input_redirect || current.output_redirect) {
        pipeline_add_command(pipeline, &current);
    } else {
        free_command(&current);
    }

    return true;
}

static void remove_job_index(size_t index) {
    if (index >= job_count) {
        return;
    }
    free(jobs[index].command);
    for (size_t i = index + 1; i < job_count; ++i) {
        jobs[i - 1] = jobs[i];
    }
    job_count--;
}

static void add_job(pid_t pgid, const char *command) {
    if (job_capacity == 0) {
        job_capacity = INITIAL_CAPACITY;
        jobs = xrealloc(NULL, job_capacity * sizeof(Job));
    } else if (job_count == job_capacity) {
        job_capacity *= 2;
        jobs = xrealloc(jobs, job_capacity * sizeof(Job));
    }
    jobs[job_count].id = next_job_id++;
    jobs[job_count].pgid = pgid;
    jobs[job_count].command = xstrdup(command);
    jobs[job_count].state = JOB_RUNNING;
    job_count++;
}

static Job *find_job_by_id(int id) {
    for (size_t i = 0; i < job_count; ++i) {
        if (jobs[i].id == id) {
            return &jobs[i];
        }
    }
    return NULL;
}

static void reap_jobs(void) {
    for (size_t i = 0; i < job_count;) {
        int status = 0;
        pid_t result = waitpid(-jobs[i].pgid, &status, WNOHANG);
        if (result == 0) {
            ++i;
            continue;
        }
        if (result == -1 && errno == ECHILD) {
            printf("[%d] Done\t%s\n", jobs[i].id, jobs[i].command);
            remove_job_index(i);
            continue;
        }
        if (result > 0) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                printf("[%d] Done\t%s\n", jobs[i].id, jobs[i].command);
                remove_job_index(i);
                continue;
            }
        }
        ++i;
    }
}

static int builtin_cd(Command *cmd) {
    const char *target = cmd->argc > 1 ? cmd->argv[1] : getenv("HOME");
    if (!target) {
        target = ".";
    }
    if (chdir(target) == -1) {
        perror("cd");
        return 1;
    }
    return 0;
}

static int builtin_pwd(void) {
    char buffer[PATH_MAX];
    if (!getcwd(buffer, sizeof(buffer))) {
        perror("pwd");
        return 1;
    }
    printf("%s\n", buffer);
    return 0;
}

static int builtin_mkdir(Command *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "mkdir: missing operand\n");
        return 1;
    }
    int exit_code = 0;
    for (size_t i = 1; i < cmd->argc; ++i) {
        if (mkdir(cmd->argv[i], 0755) == -1) {
            perror("mkdir");
            exit_code = 1;
        }
    }
    return exit_code;
}

static int builtin_touch(Command *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "touch: missing file operand\n");
        return 1;
    }
    int exit_code = 0;
    for (size_t i = 1; i < cmd->argc; ++i) {
        int fd = open(cmd->argv[i], O_CREAT | O_WRONLY, 0644);
        if (fd == -1) {
            perror("touch");
            exit_code = 1;
            continue;
        }
        close(fd);
    }
    return exit_code;
}

static int builtin_history(void) {
    for (size_t i = 0; i < history.count; ++i) {
        printf("%zu  %s\n", i + 1, history.items[i]);
    }
    return 0;
}

static int builtin_jobs(void) {
    reap_jobs();
    for (size_t i = 0; i < job_count; ++i) {
        const char *status = jobs[i].state == JOB_RUNNING ? "Running" :
                             jobs[i].state == JOB_STOPPED ? "Stopped" : "Done";
        printf("[%d] %s\t%s\n", jobs[i].id, status, jobs[i].command);
    }
    return 0;
}

static int builtin_fg(Command *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "fg: usage fg <job-id>\n");
        return 1;
    }
    int job_id = atoi(cmd->argv[1]);
    Job *job = find_job_by_id(job_id);
    if (!job) {
        fprintf(stderr, "fg: no such job %d\n", job_id);
        return 1;
    }
    if (kill(-job->pgid, SIGCONT) == -1) {
        perror("fg");
        return 1;
    }
    int status = 0;
    if (waitpid(-job->pgid, &status, 0) == -1) {
        perror("waitpid");
    }
    remove_job_index(job - jobs);
    return 0;
}

static int builtin_bg(Command *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "bg: usage bg <job-id>\n");
        return 1;
    }
    int job_id = atoi(cmd->argv[1]);
    Job *job = find_job_by_id(job_id);
    if (!job) {
        fprintf(stderr, "bg: no such job %d\n", job_id);
        return 1;
    }
    if (kill(-job->pgid, SIGCONT) == -1) {
        perror("bg");
        return 1;
    }
    job->state = JOB_RUNNING;
    return 0;
}

static int identify_builtin(const Command *cmd) {
    if (cmd->argc == 0) {
        return -1;
    }
    const char *name = cmd->argv[0];
    if (strcmp(name, "cd") == 0) return 0;
    if (strcmp(name, "pwd") == 0) return 1;
    if (strcmp(name, "exit") == 0) return 2;
    if (strcmp(name, "mkdir") == 0) return 3;
    if (strcmp(name, "touch") == 0) return 4;
    if (strcmp(name, "history") == 0) return 5;
    if (strcmp(name, "jobs") == 0) return 6;
    if (strcmp(name, "fg") == 0) return 7;
    if (strcmp(name, "bg") == 0) return 8;
    return -1;
}

static int execute_builtin(Command *cmd) {
    int which = identify_builtin(cmd);
    switch (which) {
        case 0: return builtin_cd(cmd);
        case 1: return builtin_pwd();
        case 2: exit(0);
        case 3: return builtin_mkdir(cmd);
        case 4: return builtin_touch(cmd);
        case 5: return builtin_history();
        case 6: return builtin_jobs();
        case 7: return builtin_fg(cmd);
        case 8: return builtin_bg(cmd);
        default: return -1;
    }
}

static void setup_redirection(const Command *cmd) {
    if (cmd->input_redirect) {
        int fd = open(cmd->input_redirect, O_RDONLY);
        if (fd == -1) {
            perror("open");
            exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2");
            exit(EXIT_FAILURE);
        }
        close(fd);
    }
    if (cmd->output_redirect) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->output_redirect, flags, 0644);
        if (fd == -1) {
            perror("open");
            exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDOUT_FILENO) == -1) {
            perror("dup2");
            exit(EXIT_FAILURE);
        }
        close(fd);
    }
}

static void execute_child(Command *cmd) {
    int builtin = identify_builtin(cmd);
    if (builtin >= 0) {
        execute_builtin(cmd);
        exit(EXIT_SUCCESS);
    }
    if (execvp(cmd->argv[0], cmd->argv) == -1) {
        perror("execvp");
        exit(EXIT_FAILURE);
    }
}

static pid_t launch_pipeline(Pipeline *pipeline, const char *raw_command, bool wait_for_children) {
    size_t n = pipeline->count;
    int (*pipes)[2] = NULL;
    pid_t *pids = xrealloc(NULL, n * sizeof(pid_t));

    if (n > 1) {
        pipes = xrealloc(NULL, (n - 1) * sizeof(int[2]));
        for (size_t i = 0; i < n - 1; ++i) {
            if (pipe(pipes[i]) == -1) {
                die("pipe");
            }
        }
    }

    pid_t pgid = 0;
    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == -1) {
            die("fork");
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);

            if (pgid == 0) {
                pgid = getpid();
            }
            setpgid(0, pgid);

            if (n > 1) {
                if (i > 0) {
                    if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                        die("dup2");
                    }
                }
                if (i < n - 1) {
                    if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                        die("dup2");
                    }
                }
                for (size_t j = 0; j < n - 1; ++j) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
            }

            setup_redirection(&pipeline->commands[i]);
            execute_child(&pipeline->commands[i]);
        } else {
            if (pgid == 0) {
                pgid = pid;
            }
            setpgid(pid, pgid);
            pids[i] = pid;
        }
    }

    if (n > 1) {
        for (size_t i = 0; i < n - 1; ++i) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }
        free(pipes);
    }

    if (wait_for_children) {
        int status = 0;
        for (size_t i = 0; i < n; ++i) {
            waitpid(pids[i], &status, 0);
        }
    } else {
        add_job(pgid, raw_command);
        printf("[%d] %d\n", next_job_id - 1, pgid);
    }

    free(pids);
    return pgid;
}

static void handle_signal(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
}

static void prompt(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("%s $ ", cwd);
    } else {
        printf("$ ");
    }
    fflush(stdout);
}

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTSTP, handle_signal);
    signal(SIGQUIT, SIG_IGN);

    char *line = NULL;
    size_t len = 0;

    while (1) {
        reap_jobs();
        prompt();
        ssize_t read = getline(&line, &len, stdin);
        if (read == -1) {
            printf("\n");
            break;
        }
        if (read > 0 && line[read - 1] == '\n') {
            line[read - 1] = '\0';
        }
        if (line[0] == '\0') {
            continue;
        }
        add_history_entry(line);

        size_t token_count = 0;
        char **tokens = tokenize(line, &token_count);
        if (!tokens) {
            continue;
        }

        Pipeline pipeline = {0};
        if (!parse_pipeline(tokens, token_count, &pipeline)) {
            free_tokens(tokens, token_count);
            continue;
        }

        bool run_in_parent = !pipeline.background && pipeline.count == 1 && identify_builtin(&pipeline.commands[0]) >= 0;
        if (run_in_parent) {
            execute_builtin(&pipeline.commands[0]);
        } else {
            launch_pipeline(&pipeline, line, !pipeline.background);
        }

        free_pipeline(&pipeline);
        free_tokens(tokens, token_count);
    }

    free(line);
    for (size_t i = 0; i < history.count; ++i) {
        free(history.items[i]);
    }
    free(history.items);
    for (size_t i = 0; i < job_count; ++i) {
        free(jobs[i].command);
    }
    free(jobs);
    return 0;
}

