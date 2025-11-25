// so.c
// sistema operacional
// simulador de computador
// so25b

// ---------------------------------------------------------------------
// INCLUDES {{{1
// ---------------------------------------------------------------------

#include "so.h"
#include "dispositivos.h"
#include "err.h"
#include "irq.h"
#include "memoria.h"
#include "programa.h"

#include <stdlib.h>
#include <stdbool.h>


// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas

// Estrutura para métricas globais
typedef struct {
  long tempo_total_execucao;
  long tempo_total_ocioso;
  int num_processos_criados;
  int num_preempcoes_total;
  int num_irq[N_IRQ];
  long inicio_tempo_ocioso; // Para calcular o tempo ocioso
} metricas_globais_t;

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  es_t *es;
  console_t *console;
  bool erro_interno;

  // Estado dos processos
  processo_t tabela_processos[MAX_PROCESSOS];
  int processo_em_execucao_idx; // Índice na tabela_processos, ou -1 se nenhum
  int proximo_pid;              // Próximo PID a ser alocado

  // --- Campos de Escalonamento e Métricas (Parte III) ---

  // Fila de prontos para Round-Robin
  int fila_prontos[MAX_PROCESSOS];
  int fila_prontos_inicio;
  int fila_prontos_fim;
  int fila_prontos_tamanho;

  // Controle do Quantum
  int quantum_total;        // Nº de interrupções de relógio por quantum
  int quantum_restante;     // Quantum restante do processo atual
  bool deve_preemptar;      // Flag setada pela IRQ do relógio

  // Seleção do Escalonador
  tipo_escalonador_t escalonador_atual;

  // Métricas
  metricas_globais_t metricas;
};


// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender);
// retorna o tempo atual do sistema (número de instruções)
static int so_get_tempo(so_t *self);
// inicializa os campos de métricas de um novo processo
static void so_inicializa_metricas_processo(so_t *self, processo_t *proc, int pid, int ender_carga);
// funções da fila de prontos (Round-Robin)
static void fila_prontos_insere(so_t *self, int idx_proc);
static int fila_prontos_remove(so_t *self);
static void so_insere_em_pronto(so_t *self, int idx_proc);
// atualiza o estado de um processo e registra métricas
static void so_atualiza_estado(so_t *self, processo_t *proc, estado_processo_t novo_estado);
static void so_registra_preempcao(so_t *self, processo_t *proc);


// ---------------------------------------------------------------------
// FUNÇÕES AUXILIARES - TEMPO {{{1
// ---------------------------------------------------------------------

// Retorna o tempo atual do sistema (número de instruções)
static int so_get_tempo(so_t *self)
{
  int tempo;
  if (es_le(self->es, D_RELOGIO_INSTRUCOES, &tempo) != ERR_OK) {
    console_printf("SO: Falha ao ler relógio de instruções!");
    return 0;
  }
  return tempo;
}


// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->es = es;
  self->console = console;
  self->erro_interno = false;

  // Inicializa controle de processos
  self->processo_em_execucao_idx = -1;
  self->proximo_pid = 1;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    self->tabela_processos[i].estado = LIVRE;
  }

  // Inicializa fila de prontos (RR)
  self->fila_prontos_inicio = 0;
  self->fila_prontos_fim = 0;
  self->fila_prontos_tamanho = 0;

  // Inicializa escalonador (Padrão: RR com Quantum 3)
  self->escalonador_atual = ESCAL_CIRCULAR; // Mude para ESCAL_PRIORIDADE para testar o outro
  self->quantum_total = 3; // Quantum = 3 interrupções de relógio
  self->quantum_restante = 0;
  self->deve_preemptar = false;

  // Inicializa métricas
  self->metricas.tempo_total_execucao = 0;
  self->metricas.tempo_total_ocioso = 0;
  self->metricas.num_processos_criados = 0;
  self->metricas.num_preempcoes_total = 0;
  self->metricas.inicio_tempo_ocioso = 0;
  for (int i = 0; i < N_IRQ; i++) {
    self->metricas.num_irq[i] = 0;
  }

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
}


// ---------------------------------------------------------------------
// TRATAMENTO DE INTERRUPÇÃO {{{1
// ---------------------------------------------------------------------

// funções auxiliares para o tratamento de interrupção
static void so_salva_estado_da_cpu(so_t *self);
static void so_trata_irq(so_t *self, int irq);
static void so_trata_pendencias(so_t *self);
static void fila_prontos_insere(so_t *self, int idx_proc);
static int fila_prontos_remove(so_t *self);
static void so_atualiza_estado(so_t *self, processo_t *proc, estado_processo_t novo_estado);
static void so_registra_preempcao(so_t *self, processo_t *proc);
static void so_calcula_prioridade(so_t *self, processo_t *proc);
static void so_escalona_rr(so_t *self);
static void so_escalona_prio(so_t *self);
static void so_escalona(so_t *self);
static int so_despacha(so_t *self);
static void so_imprime_relatorio_final(so_t *self);

// função a ser chamada pela CPU quando executa a instrução CHAMAC, no tratador de
//   interrupção em assembly
// essa é a única forma de entrada no SO depois da inicialização
// na inicialização do SO, a CPU foi programada para chamar esta função para executar
//   a instrução CHAMAC
// a instrução CHAMAC só deve ser executada pelo tratador de interrupção
//
// o primeiro argumento é um ponteiro para o SO, o segundo é a identificação
//   da interrupção
// o valor retornado por esta função é colocado no registrador A, e pode ser
//   testado pelo código que está após o CHAMAC. No tratador de interrupção em
//   assembly esse valor é usado para decidir se a CPU deve retornar da interrupção
//   (e executar o código de usuário) ou executar PARA e ficar suspensa até receber
//   outra interrupção
static int so_trata_interrupcao(void *argC, int reg_A)
{
  so_t *self = argC;
  irq_t irq = reg_A;
  // esse print polui bastante, recomendo tirar quando estiver com mais confiança
  console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  so_escalona(self);
  // recupera o estado do processo escolhido
  return so_despacha(self);
}

static void so_salva_estado_da_cpu(so_t *self)
{
  // Se não houver processo corrente, não faz nada
  if (self->processo_em_execucao_idx == -1) {
    return;
  }

  // Obtém o ponteiro para o PCB do processo atual
  processo_t *proc = &self->tabela_processos[self->processo_em_execucao_idx];

  // Salva os registradores da memória para o PCB
  if (mem_le(self->mem, CPU_END_A, &proc->estado_cpu.regA) != ERR_OK
      || mem_le(self->mem, CPU_END_PC, &proc->estado_cpu.regPC) != ERR_OK
      || mem_le(self->mem, CPU_END_erro, &proc->estado_cpu.regERRO) != ERR_OK
      || mem_le(self->mem, 59, &proc->estado_cpu.regX)) { // regX salvo em 59 por trata_int.asm
    console_printf("SO: erro na leitura dos registradores para o PCB");
    self->erro_interno = true;
  }
}

static void so_trata_pendencias(so_t *self)
{
  // Itera por toda a tabela de processos procurando por processos
  // bloqueados por E/S que possam ser desbloqueados.

  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *proc = &self->tabela_processos[i];

    if (proc->estado != BLOQUEADO) {
      continue; // Só nos importam os bloqueados
    }

    if (proc->motivo_bloqueio == BLOQUEIO_IO_LE) {
      // --- Trata pendência de LEITURA ---
      int term = proc->dispositivo_esperado;
      int estado;
      if (es_le(self->es, term + TERM_TECLADO_OK, &estado) != ERR_OK) {
        console_printf("SO (pend): erro ao ler estado teclado (proc %d)", proc->pid);
        so_atualiza_estado(self, proc, LIVRE); // Mata o processo
        continue;
      }

      if (estado != 0) {
        // Dispositivo pronto! Completar a operação.
        int dado;
        if (es_le(self->es, term + TERM_TECLADO, &dado) != ERR_OK) {
          console_printf("SO (pend): erro ao ler teclado (proc %d)", proc->pid);
          so_atualiza_estado(self, proc, LIVRE);
          continue;
        }

        proc->estado_cpu.regA = dado; // Define o retorno da syscall
        so_atualiza_estado(self, proc, PRONTO);
        proc->motivo_bloqueio = BLOQUEIO_NENHUM;
        so_insere_em_pronto(self, i);
        console_printf("SO: Processo %d desbloqueado por E/S (leitura)", proc->pid);
      }

    } else if (proc->motivo_bloqueio == BLOQUEIO_IO_ESCR) {
      // --- Trata pendência de ESCRITA ---
      int term = proc->dispositivo_esperado;
      int estado;
      if (es_le(self->es, term + TERM_TELA_OK, &estado) != ERR_OK) {
        console_printf("SO (pend): erro ao ler estado tela (proc %d)", proc->pid);
        so_atualiza_estado(self, proc, LIVRE); // Mata o processo
        continue;
      }

      if (estado != 0) {
        // Dispositivo pronto! Completar a operação.
        int dado = proc->estado_cpu.regX; // Pega o dado a escrever (do contexto salvo)
        if (es_escreve(self->es, term + TERM_TELA, dado) != ERR_OK) {
          console_printf("SO (pend): erro ao escrever tela (proc %d)", proc->pid);
          so_atualiza_estado(self, proc, LIVRE);
          continue;
        }

        proc->estado_cpu.regA = 0; // Define o retorno da syscall (sucesso)
        so_atualiza_estado(self, proc, PRONTO);
        proc->motivo_bloqueio = BLOQUEIO_NENHUM;
        so_insere_em_pronto(self, i);
        console_printf("SO: Processo %d desbloqueado por E/S (escrita)", proc->pid);
      }
    }
  }
}

// --- Helpers da Fila de Prontos (RR) ---
static void fila_prontos_insere(so_t *self, int idx_proc)
{
  if (self->fila_prontos_tamanho == MAX_PROCESSOS) {
    console_printf("SO: Fila de prontos cheia!");
    return; // ou erro fatal
  }
  self->fila_prontos[self->fila_prontos_fim] = idx_proc;
  self->fila_prontos_fim = (self->fila_prontos_fim + 1) % MAX_PROCESSOS;
  self->fila_prontos_tamanho++;
}

static int fila_prontos_remove(so_t *self)
{
  if (self->fila_prontos_tamanho == 0) {
    return -1; // Fila vazia
  }
  int idx_proc = self->fila_prontos[self->fila_prontos_inicio];
  self->fila_prontos_inicio = (self->fila_prontos_inicio + 1) % MAX_PROCESSOS;
  self->fila_prontos_tamanho--;
  return idx_proc;
}
// --- Fim Helpers Fila ---

// Insere processo na estrutura de PRONTOS de acordo com o escalonador
static void so_insere_em_pronto(so_t *self, int idx_proc)
{
  if (self->escalonador_atual == ESCAL_CIRCULAR) {
    fila_prontos_insere(self, idx_proc);
  }
}

// Atualiza o estado de um processo e registra as métricas de tempo
static void so_atualiza_estado(so_t *self, processo_t *proc, estado_processo_t novo_estado)
{
  int tempo_agora = so_get_tempo(self);
  estado_processo_t estado_antigo = proc->estado;

  if (estado_antigo == novo_estado) return; // Nada a fazer

  // Calcula tempo gasto no estado antigo
  int tempo_no_estado_antigo = tempo_agora - proc->ultimo_tempo_mudanca_estado;
  if (tempo_no_estado_antigo > 0) {
      proc->tempo_total_estado[estado_antigo] += tempo_no_estado_antigo;
  }

  // Atualiza para o novo estado
  proc->estado = novo_estado;
  proc->contagem_estado[novo_estado]++;
  proc->ultimo_tempo_mudanca_estado = tempo_agora;

  // Se entrou em PRONTO, marca o tempo
  if (novo_estado == PRONTO) {
    proc->ultimo_tempo_pronto = tempo_agora;
  }

  // Se saiu de PRONTO para EXECUTANDO, calcula o tempo de resposta
  if (estado_antigo == PRONTO && novo_estado == EXECUTANDO) {
    proc->tempo_total_pronto += (tempo_agora - proc->ultimo_tempo_pronto);
  }

  if ((novo_estado == TERMINADO || novo_estado == LIVRE) && proc->tempo_termino < 0 && estado_antigo != LIVRE) {
    proc->tempo_termino = tempo_agora;
  }
}

static void so_registra_preempcao(so_t *self, processo_t *proc)
{
  if (proc == NULL) {
    return;
  }
  proc->num_preempcoes++;
  self->metricas.num_preempcoes_total++;
}

// Calcula a nova prioridade de um processo ao bloquear ou ser preemptado
static void so_calcula_prioridade(so_t *self, processo_t *proc)
{
  // prio = (prio + t_exec/t_quantum) / 2
  int t_exec = self->quantum_total - self->quantum_restante;
  if (t_exec < 0) t_exec = 0; // Caso tenha bloqueado antes do quantum começar

  float percentual_usado = (float)t_exec / (float)self->quantum_total;
  proc->prioridade = (proc->prioridade + percentual_usado) / 2.0;

  console_printf("SO: Nova prioridade do proc %d: %.2f", proc->pid, proc->prioridade);
}

// Escalonador 1: Round-Robin (Circular)
static void so_escalona_rr(so_t *self)
{
  int idx_atual = self->processo_em_execucao_idx;
  processo_t *proc_atual = (idx_atual != -1) ? &self->tabela_processos[idx_atual] : NULL;

  // 1. Lógica de Preempção
  if (self->deve_preemptar && proc_atual != NULL) {
    console_printf("SO: Preempção RR do processo %d", proc_atual->pid);
    so_registra_preempcao(self, proc_atual);
    so_atualiza_estado(self, proc_atual, PRONTO);
    so_insere_em_pronto(self, idx_atual);
    proc_atual = NULL; // Força a escolha de um novo processo
    self->processo_em_execucao_idx = -1;
  }
  self->deve_preemptar = false; // Limpa a flag

  // 2. Verifica se o processo atual bloqueou ou terminou
  if (proc_atual != NULL && proc_atual->estado != EXECUTANDO) {
    // O processo bloqueou, terminou ou foi preemptado
    proc_atual = NULL;
    self->processo_em_execucao_idx = -1;
  }

  // 3. Se o processo atual continua (não foi preemptado nem bloqueou), não faz nada
  if (proc_atual != NULL) {
    return;
  }

  // 4. Seleciona o próximo processo da fila
  int proximo_idx = fila_prontos_remove(self);

  if (proximo_idx != -1) {
    // --- Achou um processo pronto ---
    if (self->metricas.inicio_tempo_ocioso > 0) {
      // Estava ocioso, agora não está mais
      self->metricas.tempo_total_ocioso += (so_get_tempo(self) - self->metricas.inicio_tempo_ocioso);
      self->metricas.inicio_tempo_ocioso = 0;
    }

    processo_t *proximo_proc = &self->tabela_processos[proximo_idx];
    so_atualiza_estado(self, proximo_proc, EXECUTANDO);
    self->processo_em_execucao_idx = proximo_idx;
    self->quantum_restante = self->quantum_total; // Reseta o quantum
    console_printf("SO: Escalonou %d (RR)", proximo_proc->pid);
  } else {
    // --- Fila vazia, sistema ocioso ---
    self->processo_em_execucao_idx = -1;
    if (self->metricas.inicio_tempo_ocioso == 0) {
      // Começou a ficar ocioso agora
      self->metricas.inicio_tempo_ocioso = so_get_tempo(self);
      console_printf("SO: Nenhum processo pronto. Entrando em modo ocioso.");
    }
  }
}

// Escalonador 2: Prioridade
static void so_escalona_prio(so_t *self)
{
  int idx_atual = self->processo_em_execucao_idx;
  processo_t *proc_atual = (idx_atual != -1) ? &self->tabela_processos[idx_atual] : NULL;

  // Trata processo em execução
  if (proc_atual != NULL) {
    if (proc_atual->estado == BLOQUEADO || proc_atual->estado == TERMINADO) {
      // Se bloqueou ou terminou, recalcula prioridade
      so_calcula_prioridade(self, proc_atual);
      self->processo_em_execucao_idx = -1;
      proc_atual = NULL;
    } else if (self->deve_preemptar) {
      // Se quantum acabou, recalcula prioridade e insere na fila
      so_calcula_prioridade(self, proc_atual);
      so_registra_preempcao(self, proc_atual);
      so_atualiza_estado(self, proc_atual, PRONTO);
      console_printf("SO: Processo %d preemptado por fim de quantum.", proc_atual->pid);
      self->processo_em_execucao_idx = -1;
      proc_atual = NULL;
    }
    self->deve_preemptar = false;
  }

  // Se não tem processo em execução, escolhe o de menor prioridade
  if (self->processo_em_execucao_idx == -1) {
    int melhor_idx = -1;
    float melhor_prio = 1e9; // Inicia com valor alto

    for (int i = 0; i < MAX_PROCESSOS; i++) {
      processo_t *p = &self->tabela_processos[i];
      if (p->estado == PRONTO) {
        if (p->prioridade < melhor_prio) {
          melhor_prio = p->prioridade;
          melhor_idx = i;
        }
      }
    }

    if (melhor_idx != -1) {
      // Achou um processo pronto
      processo_t *novo_proc = &self->tabela_processos[melhor_idx];
      // Se estava ocioso, registra fim de ociosidade
      if (self->metricas.inicio_tempo_ocioso > 0) {
        int tempo_fim_ocioso = so_get_tempo(self);
        self->metricas.tempo_total_ocioso += (tempo_fim_ocioso - self->metricas.inicio_tempo_ocioso);
        self->metricas.inicio_tempo_ocioso = 0;
      }

      so_atualiza_estado(self, novo_proc, EXECUTANDO);
      self->processo_em_execucao_idx = melhor_idx;
      self->quantum_restante = self->quantum_total;
      console_printf("SO: Processo %d selecionado para execução (prioridade: %.2f)", novo_proc->pid, novo_proc->prioridade);
    } else {
      // Nenhum processo pronto
      self->processo_em_execucao_idx = -1;
      if (self->metricas.inicio_tempo_ocioso == 0) {
        self->metricas.inicio_tempo_ocioso = so_get_tempo(self);
        console_printf("SO: Nenhum processo pronto. Entrando em modo ocioso.");
      }
    }
  }
}

static void so_escalona(so_t *self)
{
  // Despacha para o escalonador configurado
  if (self->escalonador_atual == ESCAL_CIRCULAR) {
    so_escalona_rr(self);
  } else {
    // ESCAL_PRIORIDADE
    so_escalona_prio(self);
  }
}

static int so_despacha(so_t *self)
{
  // Se não houver processo para executar, retorna 1 (HALT)
  if (self->processo_em_execucao_idx == -1) {
    console_printf("SO: Nenhum processo pronto. CPU em HALT.");
    return 1; // Diz ao trata_int.asm para executar PARA
  }

  // Obtém o ponteiro para o PCB do processo que vai executar
  processo_t *proc = &self->tabela_processos[self->processo_em_execucao_idx];

  // Escreve os registradores do PCB para a memória
  if (mem_escreve(self->mem, CPU_END_A, proc->estado_cpu.regA) != ERR_OK
      || mem_escreve(self->mem, CPU_END_PC, proc->estado_cpu.regPC) != ERR_OK
      || mem_escreve(self->mem, CPU_END_erro, proc->estado_cpu.regERRO) != ERR_OK
      || mem_escreve(self->mem, 59, proc->estado_cpu.regX)) {
    console_printf("SO: erro na escrita dos registradores do PCB");
    self->erro_interno = true;
    return 1; // Erro, melhor parar
  }

  return 0; // Diz ao trata_int.asm para executar RETI
}


// ---------------------------------------------------------------------
// TRATAMENTO DE UMA IRQ {{{1
// ---------------------------------------------------------------------

// funções auxiliares para tratar cada tipo de interrupção
static void so_trata_reset(so_t *self);
static void so_trata_irq_chamada_sistema(so_t *self);
static void so_trata_irq_err_cpu(so_t *self);
static void so_trata_irq_relogio(so_t *self);
static void so_trata_irq_desconhecida(so_t *self, int irq);

static void so_trata_irq(so_t *self, int irq)
{
  // --- Métricas (Parte III) ---
  if (irq >= 0 && irq < N_IRQ) {
    self->metricas.num_irq[irq]++;
  }
  // --- Fim Métricas ---

  // verifica o tipo de interrupção que está acontecendo, e atende de acordo
  switch (irq) {
    case IRQ_RESET:
      so_trata_reset(self);
      break;
    case IRQ_SISTEMA:
      so_trata_irq_chamada_sistema(self);
      break;
    case IRQ_ERR_CPU:
      so_trata_irq_err_cpu(self);
      break;
    case IRQ_RELOGIO:
      so_trata_irq_relogio(self);
      break;
    default:
      so_trata_irq_desconhecida(self, irq);
  }
}

// chamada uma única vez, quando a CPU inicializa
static void so_trata_reset(so_t *self)
{
  // coloca o tratador de interrupção na memória
  // quando a CPU aceita uma interrupção, passa para modo supervisor,
  //   salva seu estado à partir do endereço CPU_END_PC, e desvia para o
  //   endereço CPU_END_TRATADOR
  // colocamos no endereço CPU_END_TRATADOR o programa de tratamento
  //   de interrupção (escrito em asm). esse programa deve conter a
  //   instrução CHAMAC, que vai chamar so_trata_interrupcao (como
  //   foi definido na inicialização do SO)
  int ender = so_carrega_programa(self, "trata_int.maq");
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // coloca o programa init na memória
  ender = so_carrega_programa(self, "init.maq");
  if (ender != 100) {
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }

  // T2: Cria o primeiro processo (init)
  processo_t *proc_init = &self->tabela_processos[0];
  so_inicializa_metricas_processo(self, proc_init, self->proximo_pid++, ender);
  proc_init->terminal = D_TERM_A; // Processo 0 usa o Terminal A

  // Adiciona na fila de prontos (para RR)
  // (Implementaremos a fila no próximo prompt, por enquanto só adicionamos)
  so_insere_em_pronto(self, 0);

  // A interrupção do BIOS não define um processo_em_execucao_idx.
  // O escalonador será chamado pela primeira vez ao fim de
  // so_trata_interrupcao e vai selecionar este processo.
}

// interrupção gerada quando a CPU identifica um erro
static void so_trata_irq_err_cpu(so_t *self)
{
  if (self->processo_em_execucao_idx != -1) {
    // so_salva_estado_da_cpu() já foi chamado, então o erro está no PCB
    processo_t *proc = &self->tabela_processos[self->processo_em_execucao_idx];
    err_t err = proc->estado_cpu.regERRO;

    console_printf("SO: Erro na CPU (processo %d): %s. Processo terminado.",
                   proc->pid, err_nome(err));

    so_atualiza_estado(self, proc, LIVRE); // Marca para reutilização

    // O escalonador será chamado e escolherá outro processo.
  } else {
    // Erro de CPU sem processo (ex: durante o boot, antes do init)
    // Isso é um erro fatal do sistema.
    console_printf("SO: IRQ de erro fatal na CPU (sem processo corrente)!");
    self->erro_interno = true; // Para a simulação
  }
}

// interrupção gerada quando o timer expira
static void so_trata_irq_relogio(so_t *self)
{
  // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
  err_t e1, e2;
  e1 = es_escreve(self->es, D_RELOGIO_INTERRUPCAO, 0); // desliga o sinalizador de interrupção
  e2 = es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO);
  if (e1 != ERR_OK || e2 != ERR_OK) {
    console_printf("SO: problema da reinicialização do timer");
    self->erro_interno = true;
  }

  // --- Lógica de Quantum (Parte III) ---
  if (self->processo_em_execucao_idx != -1) {
    self->quantum_restante--;
    if (self->quantum_restante == 0) {
      processo_t *proc = &self->tabela_processos[self->processo_em_execucao_idx];

      console_printf("SO: Quantum do processo %d estourou (Preempção)", proc->pid);
      self->deve_preemptar = true;
    }
  }
}

// foi gerada uma interrupção para a qual o SO não está preparado
static void so_trata_irq_desconhecida(so_t *self, int irq)
{
  console_printf("SO: não sei tratar IRQ %d (%s)", irq, irq_nome(irq));
  self->erro_interno = true;
}


// ---------------------------------------------------------------------
// CHAMADAS DE SISTEMA {{{1
// ---------------------------------------------------------------------

// funções auxiliares para cada chamada de sistema
static void so_chamada_le(so_t *self);
static void so_chamada_escr(so_t *self);
static void so_chamada_cria_proc(so_t *self);
static void so_chamada_mata_proc(so_t *self);
static void so_chamada_espera_proc(so_t *self);

static void so_trata_irq_chamada_sistema(so_t *self)
{
  // a identificação da chamada está no registrador A
  // t2: com processos, o reg A deve estar no descritor do processo corrente
  
  if (self->processo_em_execucao_idx == -1) {
    console_printf("SO: chamada de sistema sem processo em execução");
    self->erro_interno = true;
    return;
  }
  
  processo_t *proc = &self->tabela_processos[self->processo_em_execucao_idx];
  int id_chamada = proc->estado_cpu.regA;
  console_printf("SO: chamada de sistema %d", id_chamada);
  switch (id_chamada) {
    case SO_LE:
      so_chamada_le(self);
      break;
    case SO_ESCR:
      so_chamada_escr(self);
      break;
    case SO_CRIA_PROC:
      so_chamada_cria_proc(self);
      break;
    case SO_MATA_PROC:
      so_chamada_mata_proc(self);
      break;
    case SO_ESPERA_PROC:
      so_chamada_espera_proc(self);
      break;
    default:
      console_printf("SO: Processo %d fez chamada de sistema desconhecida (%d). Processo será terminado.",
                     proc->pid, id_chamada);
      // Mata o processo por chamada inválida
      so_atualiza_estado(self, proc, TERMINADO); // ou LIVRE, se preferir
      // Nota: Não podemos chamar so_chamada_mata_proc() daqui
      // porque ela pode acordar outros processos, e o chamador
      // (proc) já está sendo "morto" de forma especial.
      // O escalonador vai tirar ele de execução.
      break; // Sai do switch
  }
}

// implementação da chamada se sistema SO_LE
// faz a leitura de um dado da entrada corrente do processo, coloca o dado no reg A
static void so_chamada_le(so_t *self)
{
  if (self->processo_em_execucao_idx == -1) return;
  processo_t *proc = &self->tabela_processos[self->processo_em_execucao_idx];
  int term = proc->terminal;

  // Verifica o estado do dispositivo UMA VEZ
  int estado;
  if (es_le(self->es, term + TERM_TECLADO_OK, &estado) != ERR_OK) {
    console_printf("SO: problema no acesso ao estado do teclado (proc %d)", proc->pid);
    self->erro_interno = true;
    so_atualiza_estado(self, proc, LIVRE); // Mata o processo
    return;
  }

  if (estado != 0) {
    // --- Caminho Rápido (Dispositivo Pronto) ---
    int dado;
    if (es_le(self->es, term + TERM_TECLADO, &dado) != ERR_OK) {
      console_printf("SO: problema no acesso ao teclado (proc %d)", proc->pid);
      self->erro_interno = true;
      so_atualiza_estado(self, proc, LIVRE); // Mata o processo
      return;
    }
    proc->estado_cpu.regA = dado;
    // Processo continua PRONTO (escalonador vai rodá-lo)

  } else {
    // --- Caminho Lento (Bloqueio) ---
    console_printf("SO: Processo %d bloqueado esperando por E/S (leitura)", proc->pid);
    so_atualiza_estado(self, proc, BLOQUEADO);
    proc->motivo_bloqueio = BLOQUEIO_IO_LE;
    proc->dispositivo_esperado = term; // Armazena o terminal base
    // regA será preenchido em so_trata_pendencias
  }

  // A gambiarra console_tictac(self->console) foi removida.
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo
static void so_chamada_escr(so_t *self)
{
  if (self->processo_em_execucao_idx == -1) return;
  processo_t *proc = &self->tabela_processos[self->processo_em_execucao_idx];
  int term = proc->terminal;

  // Verifica o estado do dispositivo UMA VEZ
  int estado;
  if (es_le(self->es, term + TERM_TELA_OK, &estado) != ERR_OK) {
    console_printf("SO: problema no acesso ao estado da tela (proc %d)", proc->pid);
    self->erro_interno = true;
    so_atualiza_estado(self, proc, LIVRE); // Mata o processo
    return;
  }

  if (estado != 0) {
    // --- Caminho Rápido (Dispositivo Pronto) ---
    int dado = proc->estado_cpu.regX;
    if (es_escreve(self->es, term + TERM_TELA, dado) != ERR_OK) {
      console_printf("SO: problema no acesso à tela (proc %d)", proc->pid);
      self->erro_interno = true;
      so_atualiza_estado(self, proc, LIVRE); // Mata o processo
      return;
    }
    proc->estado_cpu.regA = 0; // Sucesso
    // Processo continua PRONTO (escalonador vai rodá-lo)

  } else {
    // --- Caminho Lento (Bloqueio) ---
    console_printf("SO: Processo %d bloqueado esperando por E/S (escrita)", proc->pid);
    so_atualiza_estado(self, proc, BLOQUEADO);
    proc->motivo_bloqueio = BLOQUEIO_IO_ESCR;
    proc->dispositivo_esperado = term; // Armazena o terminal base
    // regA será preenchido em so_trata_pendencias
  }

  // A gambiarra console_tictac(self->console) foi removida.
}

// Inicializa os campos de métricas de um novo processo
static void so_inicializa_metricas_processo(so_t *self, processo_t *proc, int pid, int ender_carga)
{
  int tempo_agora = so_get_tempo(self);

  proc->pid = pid;
  proc->estado = PRONTO; // Começa como PRONTO
  proc->prioridade = 0.5; // Padrão para escalonador de prioridade

  // Metricas
  proc->tempo_criacao = tempo_agora;
  proc->tempo_termino = -1;
  proc->num_preempcoes = 0;
  proc->tempo_total_pronto = 0;
  proc->ultimo_tempo_pronto = tempo_agora;
  proc->ultimo_tempo_mudanca_estado = tempo_agora;

  for (int i = 0; i < 5; i++) {
    proc->contagem_estado[i] = 0;
    proc->tempo_total_estado[i] = 0;
  }
  proc->contagem_estado[PRONTO] = 1; // Começa em PRONTO

  // Estado CPU
  proc->estado_cpu.regPC = ender_carga;
  proc->estado_cpu.regA = 0;
  proc->estado_cpu.regX = 0;
  proc->estado_cpu.regERRO = 0;

  // Bloqueio
  proc->motivo_bloqueio = BLOQUEIO_NENHUM;
  proc->pid_esperado = -1;
  proc->dispositivo_esperado = -1;

  self->metricas.num_processos_criados++;
}

// implementação da chamada se sistema SO_CRIA_PROC
// cria um processo
static void so_chamada_cria_proc(so_t *self)
{
  // Obtém o processo criador
  if (self->processo_em_execucao_idx == -1) return;
  processo_t *criador = &self->tabela_processos[self->processo_em_execucao_idx];

  // 1. Encontrar um slot livre na tabela de processos
  int novo_idx = -1;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].estado == LIVRE) {
      novo_idx = i;
      break;
    }
  }

  if (novo_idx == -1) {
    console_printf("SO: Limite de processos atingido.");
    criador->estado_cpu.regA = -1; // Retorna erro
    return;
  }

  // 2. Ler o nome do programa da memória do criador
  int ender_proc = criador->estado_cpu.regX; // Endereço do nome
  char nome[100];
  if (!copia_str_da_mem(100, nome, self->mem, ender_proc)) {
    console_printf("SO: Erro ao ler nome do programa para criar processo.");
    criador->estado_cpu.regA = -1;
    return;
  }

  // 3. Carregar o programa
  int ender_carga = so_carrega_programa(self, nome);
  if (ender_carga < 0) {
    console_printf("SO: Erro ao carregar programa '%s'.", nome);
    criador->estado_cpu.regA = -1;
    return;
  }

  // 4. Inicializar o PCB do novo processo
  processo_t *novo_proc = &self->tabela_processos[novo_idx];
  so_inicializa_metricas_processo(self, novo_proc, self->proximo_pid++, ender_carga);

  // Mapeia o terminal
  novo_proc->terminal = D_TERM_A + ((novo_idx % 4) * 4); 

  // Adiciona na fila de prontos (para RR)
  so_insere_em_pronto(self, novo_idx);

  // 5. Retornar o PID do novo processo no regA do criador
  criador->estado_cpu.regA = novo_proc->pid;
}

// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
  if (self->processo_em_execucao_idx == -1) return;
  processo_t *chamador = &self->tabela_processos[self->processo_em_execucao_idx];
  int pid_alvo = chamador->estado_cpu.regX; // PID do processo a matar

  if (pid_alvo == 0) {
    pid_alvo = chamador->pid;
  }

  // Encontra o processo pelo PID
  int idx_alvo = -1;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    // Não pode matar LIVRE ou TERMINADO (já morto)
    if (self->tabela_processos[i].estado != LIVRE &&
        self->tabela_processos[i].estado != TERMINADO &&
        self->tabela_processos[i].pid == pid_alvo) {
      idx_alvo = i;
      break;
    }
  }

  if (idx_alvo == -1) {
    console_printf("SO: Tentativa de matar processo inexistente ou já morto (PID %d)", pid_alvo);
    chamador->estado_cpu.regA = -1; // Erro
    return;
  }

  // Muda o estado do processo alvo para TERMINADO
  processo_t *proc_alvo = &self->tabela_processos[idx_alvo];
  so_atualiza_estado(self, proc_alvo, TERMINADO);
  console_printf("SO: Processo %d terminado.", pid_alvo);

  // --- Lógica de Desbloqueio (Parte II) ---
  // Procura por qualquer processo que estava esperando pelo pid_alvo
  bool coletado = false;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *proc_esperando = &self->tabela_processos[i];
    if (proc_esperando->estado == BLOQUEADO &&
        proc_esperando->motivo_bloqueio == BLOQUEIO_PID &&
        proc_esperando->pid_esperado == pid_alvo) {

      console_printf("SO: Desbloqueando processo %d (esperava por %d).", proc_esperando->pid, pid_alvo);
      so_atualiza_estado(self, proc_esperando, PRONTO);
      proc_esperando->motivo_bloqueio = BLOQUEIO_NENHUM;
      proc_esperando->estado_cpu.regA = 0; // Retorno de sucesso para SO_ESPERA_PROC

      // Adiciona à fila de prontos
      so_insere_em_pronto(self, i);

      // O processo foi "coletado" (reaped)
      so_atualiza_estado(self, proc_alvo, LIVRE);
      coletado = true;
      // Nota: Na vida real, só um processo deveria "coletar".
      // Para este T, vamos acordar todos e o primeiro que coletar, coletou.
      // Simplificação: só o primeiro que encontramos coleta.
      break; // Remove este break se quiser acordar todos.
    }
  }

  if (coletado) {
     console_printf("SO: Processo %d foi coletado.", pid_alvo);
  }

  chamador->estado_cpu.regA = 0; // Sucesso

  // --- Chamada do Relatório (Parte III) ---
  if (pid_alvo == 1) { // Se o processo INIT (PID 1) morreu
    so_imprime_relatorio_final(self);
    // Para o sistema (opcional, mas bom para relatórios)
    // self->erro_interno = true; // Descomente para parar a simulação
  }
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
static void so_chamada_espera_proc(so_t *self)
{
  if (self->processo_em_execucao_idx == -1) return;
  processo_t *chamador = &self->tabela_processos[self->processo_em_execucao_idx];
  int pid_alvo = chamador->estado_cpu.regX;

  // Erro: não pode esperar por si mesmo
  if (pid_alvo == chamador->pid) {
    console_printf("SO: Processo %d tentou esperar por si mesmo.", chamador->pid);
    chamador->estado_cpu.regA = -1;
    return;
  }

  // Erro: não pode esperar por PID 0 (inválido)
  if (pid_alvo <= 0) {
    console_printf("SO: Processo %d tentou esperar por PID inválido %d.", chamador->pid, pid_alvo);
    chamador->estado_cpu.regA = -1;
    return;
  }

  // Procura o processo alvo
  int idx_alvo = -1;
  bool alvo_terminado = false;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].estado != LIVRE &&
        self->tabela_processos[i].pid == pid_alvo) {
      idx_alvo = i;
      if (self->tabela_processos[i].estado == TERMINADO) {
        alvo_terminado = true;
      }
      break;
    }
  }

  // Caso 1: Processo alvo não existe (slot LIVRE ou PID não encontrado)
  if (idx_alvo == -1) {
    console_printf("SO: Processo %d tentou esperar por PID inexistente %d.", chamador->pid, pid_alvo);
    chamador->estado_cpu.regA = -1; // Erro
    return;
  }

  // Caso 2: Processo alvo já terminou (TERMINADO)
  if (alvo_terminado) {
    console_printf("SO: Processo %d esperou por PID %d (já terminado). Coletando.", chamador->pid, pid_alvo);
    so_atualiza_estado(self, &self->tabela_processos[idx_alvo], LIVRE); // "Coleta" o processo
    chamador->estado_cpu.regA = 0; // Sucesso imediato
    return;
  }

  // Caso 3: Processo alvo existe e está ativo (PRONTO, EXECUTANDO, BLOQUEADO)
  console_printf("SO: Processo %d bloqueado esperando por PID %d.", chamador->pid, pid_alvo);
  so_atualiza_estado(self, chamador, BLOQUEADO);
  chamador->motivo_bloqueio = BLOQUEIO_PID;
  chamador->pid_esperado = pid_alvo;
  // O regA não é definido agora, será definido quando for desbloqueado
}


// ---------------------------------------------------------------------
// CARGA DE PROGRAMA {{{1
// ---------------------------------------------------------------------

// carrega o programa na memória
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, char *nome_do_executavel)
{
  // NOTA T2: Cada programa deve ser montado para um endereço de carga diferente
  // para evitar conflitos de memória entre processos. Isso ocorre porque não há
  // proteção de memória nem memória virtual implementada no T2.
  // LIMITAÇÃO: Não é possível executar o mesmo programa em múltiplos processos
  // sem montá-lo para endereços diferentes.
  // SERÁ RESOLVIDO NO T3: Com a implementação de memória virtual e paginação,
  // todos os programas poderão ser montados para o endereço virtual 0, e a MMU
  // fará a tradução para endereços físicos diferentes automaticamente.

  // programa para executar na nossa CPU
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_ini = prog_end_carga(prog);
  int end_fim = end_ini + prog_tamanho(prog);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK) {
      console_printf("Erro na carga da memória, endereco %d\n", end);
      return -1;
    }
  }

  prog_destroi(prog);
  console_printf("SO: carga de '%s' em %d-%d", nome_do_executavel, end_ini, end_fim);
  return end_ini;
}


// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

// copia uma string da memória do simulador para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// t2: deveria verificar se a memória pertence ao processo
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender)
{
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    if (mem_le(mem, ender + indice_str, &caractere) != ERR_OK) {
      return false;
    }
    if (caractere < 0 || caractere > 255) {
      return false;
    }
    str[indice_str] = caractere;
    if (caractere == 0) {
      return true;
    }
  }
  // estourou o tamanho de str
  return false;
}

// Imprime o relatório final de métricas do sistema
static void so_imprime_relatorio_final(so_t *self)
{
  static const char *estado_nome[] = {
    "LIVRE", "PRONTO", "EXECUTANDO", "BLOQUEADO", "TERMINADO"
  };

  int tempo_final = so_get_tempo(self);
  self->metricas.tempo_total_execucao = tempo_final;

  if (self->metricas.inicio_tempo_ocioso > 0) {
    self->metricas.tempo_total_ocioso += (tempo_final - self->metricas.inicio_tempo_ocioso);
    self->metricas.inicio_tempo_ocioso = 0;
  }

  console_printf("\n=== Relatório Final do Sistema ===");
  console_printf("Processos criados: %d", self->metricas.num_processos_criados);
  console_printf("Tempo total: %ld ticks", self->metricas.tempo_total_execucao);
  console_printf("Tempo ocioso: %ld ticks (%.1f%%)",
                 self->metricas.tempo_total_ocioso,
                 (tempo_final > 0)
                   ? (100.0 * (float)self->metricas.tempo_total_ocioso / tempo_final)
                   : 0.0);
  console_printf("Preempções totais: %d", self->metricas.num_preempcoes_total);

  console_printf("\nInterrupções por tipo:");
  for (int i = 0; i < N_IRQ; i++) {
    console_printf("  IRQ %-2d (%-12s): %d", i, irq_nome(i), self->metricas.num_irq[i]);
  }

  console_printf("\nProcessos:");
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *p = &self->tabela_processos[i];
    if (p->pid == 0) {
      continue;
    }

    int tempo_termino = (p->tempo_termino >= 0) ? p->tempo_termino : tempo_final;
    int tempo_retorno = tempo_termino - p->tempo_criacao;
    if (tempo_retorno < 0) tempo_retorno = 0;

    int tempos_estado[5];
    for (int e = 0; e < 5; e++) {
      tempos_estado[e] = p->tempo_total_estado[e];
    }

    if (p->estado != LIVRE && p->estado != TERMINADO) {
      int tempo_atual = tempo_final - p->ultimo_tempo_mudanca_estado;
      if (tempo_atual > 0 && p->estado >= 0 && p->estado < 5) {
        tempos_estado[p->estado] += tempo_atual;
      }
    }

    float tempo_resposta = 0.0f;
    int execucoes = p->contagem_estado[EXECUTANDO];
    if (execucoes > 0) {
      tempo_resposta = (float)p->tempo_total_pronto / execucoes;
    }

    console_printf("\n  PID %-3d retorno=%d preemp=%d", p->pid, tempo_retorno, p->num_preempcoes);
    console_printf("    estados:");
    for (int e = 0; e < 5; e++) {
      console_printf("      %-10s entradas=%-3d tempo=%d", estado_nome[e],
                     p->contagem_estado[e], tempos_estado[e]);
    }
    console_printf("    resposta média: %.2f ticks", tempo_resposta);
  }

  console_printf("=== Fim do Relatório ===\n");
}

// vim: foldmethod=marker
