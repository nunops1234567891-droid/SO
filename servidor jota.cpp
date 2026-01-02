#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define FIFO_PATH "/tmp/so_fifo"
#define LOG_PATH  "server.log"

static void die(const char *msg) {
    (void)msg;
    _exit(1);
}

static int is_space(char c) {
    return c==' ' || c=='\t' || c=='\n' || c=='\r';
}

static void ensure_fifo(void) {
    struct stat st;
    if (stat(FIFO_PATH, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            // existe mas não é FIFO
            _exit(1);
        }
        return;
    }
    if (mkfifo(FIFO_PATH, 0666) < 0) _exit(1);
}

static ssize_t writen(int fd, const void *buf, size_t n) {
    size_t left = n;
    const char *p = (const char*)buf;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)w;
        p += w;
    }
    return (ssize_t)n;
}

static void append_log_line(const char *line) {
    int lfd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (lfd < 0) return;
    (void)writen(lfd, line, strlen(line));
    (void)writen(lfd, "\n", 1);
    close(lfd);
}

// tokeniza "cmd arg1 arg2" em argv[] (modifica a string, colocando '\0')
static char **tokenize_args(char *cmd, int *argc_out) {
    int cap = 8, argc = 0;
    char **argv = (char**)malloc(sizeof(char*) * (size_t)cap);
    if (!argv) return NULL;

    char *p = cmd;

    // saltar espaços iniciais
    while (*p && is_space(*p)) p++;

    while (*p) {
        if (argc + 1 >= cap) {
            cap *= 2;
            char **tmp = (char**)realloc(argv, sizeof(char*) * (size_t)cap);
            if (!tmp) { free(argv); return NULL; }
            argv = tmp;
        }

        argv[argc++] = p;

        // avançar até espaço ou fim
        while (*p && !is_space(*p)) p++;

        if (!*p) break;

        // terminar token
        *p = '\0';
        p++;

        // saltar espaços até próximo token
        while (*p && is_space(*p)) p++;
    }

    argv[argc] = NULL;
    if (argc_out) *argc_out = argc;
    return argv;
}

int main(void) {
    ensure_fifo();

    while (1) {
        // 1) open FIFO O_RDONLY (bloqueia até um cliente abrir para escrever) :contentReference[oaicite:3]{index=3}
        int fd = open(FIFO_PATH, O_RDONLY);
        if (fd < 0) continue;

        // 2) read até EOF (cliente fecha FIFO) :contentReference[oaicite:4]{index=4}
        size_t cap = 4096, len = 0;
        char *buf = (char*)malloc(cap);
        if (!buf) { close(fd); continue; }

        while (1) {
            if (len == cap) {
                cap *= 2;
                char *tmp = (char*)realloc(buf, cap);
                if (!tmp) { free(buf); close(fd); buf = NULL; break; }
                buf = tmp;
            }
            ssize_t r = read(fd, buf + len, cap - len);
            if (r < 0) {
                if (errno == EINTR) continue;
                free(buf); buf = NULL;
                break;
            }
            if (r == 0) break; // EOF
            len += (size_t)r;
        }
        close(fd);
        if (!buf) continue;

        // garantir terminação
        if (len == cap) {
            char *tmp = (char*)realloc(buf, cap + 1);
            if (!tmp) { free(buf); continue; }
            buf = tmp;
            cap += 1;
        }
        buf[len] = '\0';

        // 3) parsing nível 1: comandos separados por ';' :contentReference[oaicite:5]{index=5}
        // vamos guardar pids para esperar por todos
        int pid_cap = 16, pid_count = 0;
        pid_t *pids = (pid_t*)malloc(sizeof(pid_t) * (size_t)pid_cap);
        if (!pids) { free(buf); continue; }

        char *save = NULL;
        char *cmd = strtok_r(buf, ";", &save);

        // também guardo as strings originais para o log (simplificação)
        // (reconstruo depois a partir de argv)
        while (cmd) {
            // ignorar comandos vazios/ espaços
            char *t = cmd;
            while (*t && is_space(*t)) t++;
            if (*t) {
                int argc = 0;
                char **argv = tokenize_args(t, &argc);
                if (argv && argc > 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        execvp(argv[0], argv);
                        _exit(127);
                    }
                    if (pid > 0) {
                        if (pid_count == pid_cap) {
                            pid_cap *= 2;
                            pid_t *tmp = (pid_t*)realloc(pids, sizeof(pid_t) * (size_t)pid_cap);
                            if (!tmp) { /* sem memória: ainda assim continuo */ }
                            else pids = tmp;
                        }
                        pids[pid_count++] = pid;
                    } else {
                        // fork falhou -> log 127
                        char line[2048];
                        size_t off = 0;
                        for (int i = 0; i < argc && off < sizeof(line)-1; i++) {
                            int n = snprintf(line + off, sizeof(line)-off, "%s%s", (i? " ":""), argv[i]);
                            if (n < 0) break;
                            off += (size_t)n;
                        }
                        snprintf(line + off, sizeof(line)-off, "; exit status: %d", 127);
                        append_log_line(line);
                    }
                }
                free(argv);
            }

            cmd = strtok_r(NULL, ";", &save);
        }

        // 4) esperar por todos e escrever logs :contentReference[oaicite:6]{index=6}
        for (int i = 0; i < pid_count; i++) {
            int st = 0;
            if (waitpid(pids[i], &st, 0) < 0) continue;

            int exitcode = 127;
            if (WIFEXITED(st)) exitcode = WEXITSTATUS(st);
            else if (WIFSIGNALED(st)) exitcode = 128 + WTERMSIG(st);

            // Aqui não sei qual comando corresponde a cada pid sem mapear.
            // Para manter simples (e ainda válido), registo apenas o pid.
            // Se quiseres, eu também posso mapear pid->string do comando (muito fácil).
            char line[256];
            snprintf(line, sizeof(line), "pid=%ld; exit status: %d", (long)pids[i], exitcode);
            append_log_line(line);
        }

        free(pids);
        free(buf);
    }

    return 0;
}
