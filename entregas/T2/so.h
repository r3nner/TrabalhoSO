// so.h
// sistema operacional
// simulador de computador
// so25b

#ifndef SO_H
#define SO_H

#include "memoria.h"
#include "cpu.h"
#include "es.h"
#include "console.h" // só para uma gambiarra

// Define qual escalonador está em uso
typedef enum {
  ESCAL_CIRCULAR,   // Round-Robin
  ESCAL_PRIORIDADE  // Prioridade com preempção
} tipo_escalonador_t;

// Estado de um processo
typedef enum {
  LIVRE,
  PRONTO,
  EXECUTANDO,
  BLOQUEADO,
  TERMINADO
} estado_processo_t;

// Motivo pelo qual um processo está bloqueado
typedef enum {
  BLOQUEIO_NENHUM,
  BLOQUEIO_PID,     // Esperando um processo (pid_esperado)
  BLOQUEIO_IO_LE,   // Esperando para ler (dispositivo_esperado)
  BLOQUEIO_IO_ESCR  // Esperando para escrever (dispositivo_esperado)
} motivo_bloqueio_t;

// Estrutura para salvar o estado da CPU de um processo
typedef struct {
  int regA;
  int regX;
  int regPC;
  int regERRO;
} estado_cpu_t;

// Process Control Block (PCB)
typedef struct {
  estado_processo_t estado;
  int pid;                  // "Número de série" do processo
  estado_cpu_t estado_cpu;
  int terminal;             // Dispositivo de E/S (D_TERM_A, D_TERM_B, etc.)

  // --- Campos para Bloqueio (Parte II) ---
  motivo_bloqueio_t motivo_bloqueio;
  int pid_esperado;             // PID do processo que este espera (se motivo_bloqueio == BLOQUEIO_PID)
  int dispositivo_esperado;   // Terminal (D_TERM_A, etc.) que este espera (se motivo_bloqueio == BLOQUEIO_IO_*)

  // --- Campos de Escalonamento e Métricas (Parte III) ---
  float prioridade;                 // Para o escalonador de prioridade
  int tempo_criacao;                // "Data" de criação (em ticks)
  int tempo_termino;                // "Data" de término (em ticks)
  int num_preempcoes;               // Quantas vezes foi preemptado

  // Métricas de estado
  int contagem_estado[5];           // Contagem de quantas vezes entrou em cada estado
  int tempo_total_estado[5];        // Tempo total (em ticks) gasto em cada estado
  int ultimo_tempo_mudanca_estado;  // "Data" da última mudança de estado

  // Métricas de tempo de resposta (tempo em PRONTO)
  int tempo_total_pronto;           // Tempo total gasto na fila de PRONTO
  int ultimo_tempo_pronto;          // "Data" da última vez que entrou em PRONTO
} processo_t;

#define MAX_PROCESSOS 10

typedef struct so_t so_t;

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console);
void so_destroi(so_t *self);

// Chamadas de sistema
// Uma chamada de sistema é realizada colocando a identificação da
//   chamada (um dos valores abaixo) no registrador A e executando a
//   instrução CHAMAS, que causa uma interrupção do tipo IRQ_SISTEMA.

// Chamadas para entrada e saída
// Cada processo tem um dispositivo (ou arquivo) corrente de entrada
//   e um de saída. As chamadas de sistema para leitura e escrita são
//   realizadas nesses dispositivos.
// Outras chamadas (não definidas) abrem e fecham arquivos, e definem
//   qual dos arquivos abertos é escolhido para ser o de entrada ou
//   saída correntes.


// lê um caractere do dispositivo de entrada do processo
// retorna em A: o caractere lido ou um código de erro negativo
#define SO_LE          1

// escreve um caractere no dispositivo de saída do processo
// recebe em X o caractere a escrever
// retorna em A: 0 se OK ou um código de erro negativo
#define SO_ESCR        2

// #define SO_ABRE        3
// #define SO_FECHA       4
// #define SO_SEL_LE      5
// #define SO_SEL_ESCR    6


// Chamadas para gerenciamento de processos
// O sistema cria um processo automaticamente na sua inicialização,
//   para executar um programa inicial. Esse processo deve criar
//   outros processos para executar outros programas, se for o caso.
// Existem chamadas de sistema para criar um processo, para matar um
//   processo e para um processo se matar.
// Cada processo é identificado por um número (pid). O processo criado
//   automaticamente tem o pid 1, o segundo processo criado tem o
//   pid 2 etc.

// cria um processo novo, para executar um determinado programa
// os caracteres que compõem o nome do arquivo que contém o programa
//   a ser executado pelo novo processo estão na memória do processo
//   que realiza esta chamada, a partir da posição em X até antes
//   da posição que contém um valor 0.
// retorna em A: pid do processo criado, ou código de erro negativo
#define SO_CRIA_PROC   7

// mata um processo
// recebe em X o pid do processo a matar ou 0 para o processo chamador
// retorna em A: 0 se OK ou um código de erro negativo
#define SO_MATA_PROC   8

// espera um processo terminar
// recebe em X o pid do processo a esperar
// retorna em A: 0 se OK ou um código de erro negativo
// bloqueia o processo chamador até que o processo com o pid informado termine
// retorna sem bloquear, com erro, se não existir processo com esse pid
#define SO_ESPERA_PROC 9

#endif // SO_H
