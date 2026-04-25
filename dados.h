#ifndef DADOS_H
#define DADOS_H

#include <pthread.h>

// Estrutura com uma leitura de temperatura produzida pelo processo Sensor.
typedef struct {
    float temperatura;   // Valor da temperatura (graus Celsius).
    int contador;        // Número sequencial da leitura.
    char status[16];     // "NORMAL" ou "ALERTA".
} dado_t;

// Estrutura principal colocada dentro da memória compartilhada POSIX.
typedef struct {
    pthread_mutex_t mutex; // Mutex compartilhado entre processos.
    int inicializado;      // 0 = sem leitura válida, 1 = já existe leitura.
    dado_t dado;           // Último dado disponível para leitura.
} shm_dados_t;

// Nome da região de memória compartilhada no /dev/shm.
#define SHM_NAME "/sensor_temperatura_shm"

#endif
