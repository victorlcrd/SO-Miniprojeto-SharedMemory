#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dados.h"

// Flag global alterada pelo tratamento de sinal para encerrar o loop principal.
static volatile sig_atomic_t executando = 1;

// Trata Ctrl+C (SIGINT) para finalizar o processo de forma limpa.
static void tratar_sigint(int sig) {
    (void)sig;
    executando = 0;
}

// Gera uma temperatura pseudoaleatória no intervalo [20.00, 40.00].
static float gerar_temperatura(void) {
    return 20.0f + ((float)rand() / (float)RAND_MAX) * 20.0f;
}

int main(void) {
    // Configura tratamento de sinal para interromper o loop com segurança.
    signal(SIGINT, tratar_sigint);

    // Inicializa gerador pseudoaleatório com base no tempo atual.
    srand((unsigned int)time(NULL));

    int fd = -1;
    int criado_agora = 0;

    // Tenta criar a memória compartilhada exclusivamente para saber se é a 1ª execução.
    fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd >= 0) {
        criado_agora = 1;
    } else if (errno == EEXIST) {
        // Se já existe, apenas abre normalmente para escrita/leitura.
        fd = shm_open(SHM_NAME, O_RDWR, 0666);
    }

    if (fd < 0) {
        perror("[Sensor] Erro no shm_open");
        return 1;
    }

    // Garante que o tamanho da região seja o suficiente para shm_dados_t.
    if (ftruncate(fd, (off_t)sizeof(shm_dados_t)) == -1) {
        perror("[Sensor] Erro no ftruncate");
        close(fd);
        return 1;
    }

    // Mapeia a memória compartilhada no espaço de endereçamento do processo.
    shm_dados_t *shm = mmap(NULL, sizeof(shm_dados_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        perror("[Sensor] Erro no mmap");
        close(fd);
        return 1;
    }

    // Se a região foi criada agora, inicializa completamente mutex e conteúdo.
    if (criado_agora) {
        memset(shm, 0, sizeof(*shm));

        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

        if (pthread_mutex_init(&shm->mutex, &attr) != 0) {
            fprintf(stderr, "[Sensor] Erro ao inicializar mutex compartilhado.\n");
            pthread_mutexattr_destroy(&attr);
            munmap(shm, sizeof(*shm));
            close(fd);
            return 1;
        }

        pthread_mutexattr_destroy(&attr);
        shm->inicializado = 0;

        printf("[Sensor] Memoria compartilhada criada e mutex inicializado.\n");
    } else {
        printf("[Sensor] Memoria compartilhada existente aberta.\n");
    }

    int contador_local = 0;

    // Loop principal: gera uma leitura por segundo e publica na memória compartilhada.
    while (executando) {
        dado_t novo_dado;

        // Monta a nova leitura de temperatura.
        novo_dado.temperatura = gerar_temperatura();
        contador_local++;
        novo_dado.contador = contador_local;

        // Define o status com base na regra de limiar de 30°C.
        if (novo_dado.temperatura <= 30.0f) {
            snprintf(novo_dado.status, sizeof(novo_dado.status), "NORMAL");
        } else {
            snprintf(novo_dado.status, sizeof(novo_dado.status), "ALERTA");
        }

        printf("[Sensor] Nova leitura #%d: %.2f C - %s\n", novo_dado.contador, novo_dado.temperatura,
               novo_dado.status);

        // Primeiro tenta obter o mutex sem bloquear para registrar contenção no log.
        int r = pthread_mutex_trylock(&shm->mutex);
        if (r == 0) {
            printf("[Sensor] Mutex obtido imediatamente.\n");
        } else {
            printf("[Sensor] Mutex ocupado. Aguardando...\n");
            pthread_mutex_lock(&shm->mutex);
            printf("[Sensor] Mutex obtido apos espera.\n");
        }

        // Região crítica: atualiza o dado compartilhado e sinaliza que já há leitura válida.
#ifdef TESTE_MUTEX
        usleep(300000); // Atraso artificial para facilitar demonstração de contenção.
#endif
        shm->dado = novo_dado;
        shm->inicializado = 1;
        printf("[Sensor] Dados gravados na memoria compartilhada.\n");

        // Libera mutex para que outras threads/processos possam acessar a memória.
        pthread_mutex_unlock(&shm->mutex);
        printf("[Sensor] Mutex liberado.\n\n");

        // Aguarda aproximadamente 1 segundo antes da próxima leitura.
        sleep(1);
    }

    printf("[Sensor] Encerrando processo.\n");

    // Libera recursos locais do processo.
    munmap(shm, sizeof(*shm));
    close(fd);

    return 0;
}
