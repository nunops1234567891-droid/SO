#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define FIFO_PATH "/tmp/so_fifo"

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

static void usage(const char *p) {
    const char *msg =
        "Uso:\n"
        "  %s cmd1 [args...] -- cmd2 [args...] -- cmd3 [args...]\n"
        "Exemplo:\n"
        "  %s ls -lisa -- cp a.txt b.txt -- ps -A\n";
    char buf[512];
    int n = snprintf(buf, sizeof(buf), msg, p, p);
    if (n > 0) (void)writen(STDERR_FILENO, buf, (size_t)n);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    // construir mensagem "cmd arg;cmd arg;cmd"
    size_t cap = 4096, len = 0;
    char *msg = (char*)malloc(cap);
    if (!msg) return 1;

    int first_cmd = 1;
    int i = 1;

    while (i < argc) {
        // início de um comando
        if (!first_cmd) {
            // adicionar ';'
            if (len + 1 >= cap) {
                cap *= 2;
                char *tmp = (char*)realloc(msg, cap);
                if (!tmp) { free(msg); return 1; }
                msg = tmp;
            }
            msg[len++] = ';';
        }
        first_cmd = 0;

        // copiar tokens até '--' ou fim
        int first_tok = 1;
        while (i < argc && strcmp(argv[i], "--") != 0) {
            size_t tlen = strlen(argv[i]);
            // espaço entre args
            size_t need = tlen + (first_tok ? 0 : 1);

            while (len + need >= cap) {
                cap *= 2;
                char *tmp = (char*)realloc(msg, cap);
                if (!tmp) { free(msg); return 1; }
                msg = tmp;
            }

            if (!first_tok) msg[len++] = ' ';
            memcpy(msg + len, argv[i], tlen);
            len += tlen;

            first_tok = 0;
            i++;
        }

        // saltar o separador '--'
        if (i < argc && strcmp(argv[i], "--") == 0) i++;
    }

    // abrir FIFO e fazer 1 write() com a mensagem completa :contentReference[oaicite:9]{index=9}
    int fd = open(FIFO_PATH, O_WRONLY);
    if (fd < 0) { free(msg); return 1; }

    if (writen(fd, msg, len) < 0) { close(fd); free(msg); return 1; }

    // fechar -> EOF para o servidor :contentReference[oaicite:10]{index=10}
    close(fd);
    free(msg);
    return 0;
}

