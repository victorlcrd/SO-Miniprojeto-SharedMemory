#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "dados.h"

#define PORTA 8080
#define BACKLOG 16
#define REQ_BUFFER 1024
#define RES_BUFFER 2048

// Contexto passado para cada thread para acesso ao socket cliente e memória compartilhada.
typedef struct {
    int client_fd;
    shm_dados_t *shm;
} thread_ctx_t;

// Lê os dados compartilhados de forma protegida por mutex e envia HTML ao cliente.
static void *atender_cliente(void *arg) {
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    int client_fd = ctx->client_fd;
    shm_dados_t *shm = ctx->shm;

    // ctx foi alocado dinamicamente no accept; pode ser liberado logo no início da thread.
    free(ctx);

    // Buffer apenas para consumir a requisição HTTP recebida.
    char req[REQ_BUFFER];
    (void)recv(client_fd, req, sizeof(req) - 1, 0);

    dado_t copia;
    int tem_dado = 0;

    // Tenta obter mutex sem bloquear para log de contenção.
    int r = pthread_mutex_trylock(&shm->mutex);
    if (r == 0) {
        printf("[Thread %lu] Mutex obtido imediatamente.\n", (unsigned long)pthread_self());
    } else {
        printf("[Thread %lu] Mutex ocupado. Aguardando...\n", (unsigned long)pthread_self());
        pthread_mutex_lock(&shm->mutex);
        printf("[Thread %lu] Mutex obtido apos espera.\n", (unsigned long)pthread_self());
    }

    // Região crítica mínima: apenas copia os dados para variável local.
#ifdef TESTE_MUTEX
    usleep(300000); // Atraso artificial de teste para aumentar chance de contenção.
#endif
    if (shm->inicializado) {
        copia = shm->dado;
        tem_dado = 1;
        printf("[Thread %lu] Leitura copiada: #%d %.2f %s\n", (unsigned long)pthread_self(),
               copia.contador, copia.temperatura, copia.status);
    } else {
        printf("[Thread %lu] Ainda sem leitura do sensor.\n", (unsigned long)pthread_self());
    }

    pthread_mutex_unlock(&shm->mutex);
    printf("[Thread %lu] Mutex liberado.\n", (unsigned long)pthread_self());

    // Fora da região crítica: montagem de resposta HTTP/HTML para evitar segurar mutex por muito tempo.
    char corpo[RES_BUFFER];
    if (tem_dado) {
        snprintf(corpo, sizeof(corpo),
                 "<html>\n"
                 "<body>\n"
                 "<h2>Temperatura atual: %.2f C</h2>\n"
                 "<p>Leitura: %d</p>\n"
                 "<p>Status: %s</p>\n"
                 "</body>\n"
                 "</html>\n",
                 copia.temperatura, copia.contador, copia.status);
    } else {
        snprintf(corpo, sizeof(corpo),
                 "<html>\n"
                 "<body>\n"
                 "<h2>Sensor ainda nao enviou nenhuma leitura.</h2>\n"
                 "<p>Aguarde alguns segundos e atualize a pagina.</p>\n"
                 "</body>\n"
                 "</html>\n");
    }

    char resposta[RES_BUFFER + 256];
    int corpo_len = (int)strlen(corpo);
    int resp_len = snprintf(resposta, sizeof(resposta),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/html; charset=UTF-8\r\n"
                            "Content-Length: %d\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "%s",
                            corpo_len, corpo);

    send(client_fd, resposta, (size_t)resp_len, 0);
    close(client_fd);
    return NULL;
}

int main(void) {
    // Aguarda memória compartilhada existir (sensor deve iniciar primeiro).
    int shm_fd = -1;
    while (shm_fd < 0) {
        shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd < 0) {
            if (errno == ENOENT) {
                printf("[WebServer] Aguardando sensor criar memoria compartilhada...\n");
                sleep(1);
                continue;
            }
            perror("[WebServer] Erro no shm_open");
            return 1;
        }
    }

    // Mapeia memória compartilhada para acesso de leitura protegido por mutex.
    shm_dados_t *shm = mmap(NULL, sizeof(shm_dados_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        perror("[WebServer] Erro no mmap");
        close(shm_fd);
        return 1;
    }

    // Cria socket TCP IPv4 para atender conexões HTTP na porta 8080.
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[WebServer] Erro no socket");
        munmap(shm, sizeof(*shm));
        close(shm_fd);
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORTA);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[WebServer] Erro no bind");
        close(server_fd);
        munmap(shm, sizeof(*shm));
        close(shm_fd);
        return 1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("[WebServer] Erro no listen");
        close(server_fd);
        munmap(shm, sizeof(*shm));
        close(shm_fd);
        return 1;
    }

    printf("[WebServer] Escutando em http://localhost:%d\n", PORTA);

    // Loop principal: aceita clientes e cria uma thread POSIX para cada conexão.
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("[WebServer] Erro no accept");
            continue;
        }

        thread_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            fprintf(stderr, "[WebServer] Falha ao alocar contexto da thread.\n");
            close(client_fd);
            continue;
        }

        ctx->client_fd = client_fd;
        ctx->shm = shm;

        pthread_t tid;
        if (pthread_create(&tid, NULL, atender_cliente, ctx) != 0) {
            fprintf(stderr, "[WebServer] Falha ao criar thread para cliente.\n");
            close(client_fd);
            free(ctx);
            continue;
        }

        // Thread destacada evita necessidade de pthread_join no loop de aceitação.
        pthread_detach(tid);
    }

    // Trecho inalcançável no fluxo atual, mantido para referência/boas práticas.
    close(server_fd);
    munmap(shm, sizeof(*shm));
    close(shm_fd);

    return 0;
}
