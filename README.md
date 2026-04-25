# Miniprojeto: Sensor de Temperatura com Memória Compartilhada

Este projeto implementa **dois processos cooperantes** em C para a disciplina de Sistemas Operacionais:

- `sensor`: processo produtor (gera temperatura a cada ~1 segundo);
- `webserver`: processo consumidor multithread (publica a última leitura via HTTP).

A comunicação entre os dois processos acontece usando:

- **memória compartilhada POSIX** (`shm_open`, `ftruncate`, `mmap`);
- **mutex compartilhado entre processos** (`pthread_mutex_t` com `PTHREAD_PROCESS_SHARED`);
- **threads POSIX** no servidor (`pthread_create`) para múltiplos clientes simultâneos.

---

## 1) Estrutura de dados compartilhada

O arquivo `dados.h` define:

- `dado_t`: contém `temperatura`, `contador`, `status`;
- `shm_dados_t`: contém `mutex`, flag `inicializado` e `dado`.

A região compartilhada recebe o nome `"/sensor_temperatura_shm"`.

### Por que isso é importante?

- O `mutex` dentro da própria estrutura garante sincronização entre **processos diferentes**.
- A flag `inicializado` evita leitura de lixo antes do sensor produzir a primeira amostra.

---

## 2) Funcionamento do processo Sensor (`sensor.c`)

Fluxo principal:

1. Tenta criar a memória compartilhada (com `O_CREAT | O_EXCL`) para detectar primeira execução.
2. Ajusta o tamanho com `ftruncate`.
3. Mapeia com `mmap`.
4. Se a memória foi criada agora:
   - zera a estrutura;
   - inicializa atributos do mutex com `PTHREAD_PROCESS_SHARED`;
   - inicializa o mutex em memória compartilhada.
5. Em loop contínuo:
   - gera temperatura aleatória entre **20 e 40°C**;
   - incrementa contador;
   - define status (`NORMAL` <= 30°C, `ALERTA` > 30°C);
   - tenta `pthread_mutex_trylock` para logar se houve espera;
   - escreve dado e `inicializado = 1` na região crítica;
   - libera mutex;
   - dorme 1 segundo.

### Contenção (espera no mutex)

Quando `trylock` falha, o processo imprime:

- `Mutex ocupado. Aguardando...`
- depois usa `pthread_mutex_lock` (bloqueante);
- e registra `Mutex obtido apos espera.`

---

## 3) Funcionamento do Servidor Web (`webserver.c`)

Fluxo principal:

1. Tenta abrir a memória compartilhada criada pelo sensor.
   - Se ainda não existe, fica aguardando e avisando no log.
2. Mapeia a memória com `mmap`.
3. Cria socket TCP na porta `8080`.
4. `listen` + `accept` em loop infinito.
5. Para cada cliente, cria uma thread (`pthread_create`) e destaca com `pthread_detach`.

Fluxo da thread de atendimento:

1. Lê a requisição HTTP (apenas para consumir entrada).
2. Tenta `pthread_mutex_trylock` para log de contenção.
3. Na região crítica:
   - copia o dado compartilhado para variável local;
   - registra logs com id da thread (`pthread_self`).
4. Libera mutex.
5. **Fora da região crítica**, monta HTML e envia resposta HTTP.

### Por que copiar para variável local?

Isso reduz o tempo segurando o mutex e melhora concorrência. Assim outras threads/processos não ficam bloqueados durante montagem/envio da resposta.

---

## 4) Conceitos explicados

## 4.1 Memória compartilhada POSIX

Permite que processos distintos acessem a mesma região de memória virtual mapeada no sistema.

- `shm_open`: cria/abre o objeto compartilhado;
- `ftruncate`: define o tamanho do objeto;
- `mmap`: mapeia para ponteiro utilizável no processo.

### Vantagem

Troca de dados rápida entre processos sem serialização complexa.

### Cuidado

Como vários fluxos acessam os mesmos dados, precisa de sincronização para evitar condições de corrida.

## 4.2 Mutex compartilhado entre processos

O `pthread_mutex_t` foi colocado dentro da memória compartilhada e configurado com `PTHREAD_PROCESS_SHARED`.

Assim, tanto o `sensor` quanto threads do `webserver` usam o **mesmo lock**.

### Papel do mutex

- Garante exclusão mútua na região crítica;
- impede leituras inconsistentes durante escrita;
- evita corrupção de estado compartilhado.

## 4.3 Multithread no servidor

Cada conexão HTTP é atendida por uma thread separada.

### Benefícios

- Suporta múltiplos clientes simultâneos;
- evita fila única de atendimento bloqueando respostas.

### Cuidado

Mesmo em multithread, acesso ao dado compartilhado deve ser protegido por mutex.

## 4.4 Região crítica curta

No servidor, apenas a **cópia da leitura** fica protegida.

Montagem de HTML e `send` acontecem depois do unlock, reduzindo contenção.

---

## 5) Compilação

### Modo normal

```bash
gcc sensor.c -o sensor -lpthread -lrt
gcc webserver.c -o webserver -lpthread -lrt
```

### Modo de teste de contenção (atraso artificial)

```bash
gcc sensor.c -o sensor -lpthread -lrt -DTESTE_MUTEX
gcc webserver.c -o webserver -lpthread -lrt -DTESTE_MUTEX
```

> Em alguns Linux modernos, `-lrt` pode não ser obrigatório, mas foi mantido por compatibilidade.

---

## 6) Execução

Terminal 1:

```bash
./sensor
```

Terminal 2:

```bash
./webserver
```

Acesse:

- Navegador: `http://localhost:8080`
- ou `curl http://localhost:8080`

---

## 7) Teste de saturação

Com o servidor em execução, rode:

```bash
for i in $(seq 1 30); do
  curl -s http://localhost:8080/ > /dev/null &
done
wait
```

Resultado esperado:

- múltiplas threads atendendo ao mesmo tempo;
- logs com `Mutex ocupado. Aguardando...` em cenários de contenção.

---

## 8) Limpeza da memória compartilhada (se necessário)

Se precisar remover manualmente o objeto compartilhado:

```bash
rm /dev/shm/sensor_temperatura_shm
```

---

## 9) Observações para apresentação

- O projeto mostra **comunicação entre processos** sem arquivos e sem sockets entre eles.
- A sincronização correta com mutex compartilhado evita race condition.
- O servidor demonstra concorrência via pthread para múltiplos clientes.
- O teste com `-DTESTE_MUTEX` ajuda a visualizar claramente a contenção e a importância de região crítica curta.
