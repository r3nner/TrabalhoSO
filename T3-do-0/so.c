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
#include "tabpag.h"
#include "programa.h"
#include "vmem.h"

#include <stdlib.h>
#include <stdbool.h>


// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas
// mascara usada na aproximacao de LRU por envelhecimento
#define LRU_MSB_MASK (1UL << (sizeof(unsigned long) * 8 - 1))

// Estrutura para métricas globais
typedef struct {
  long tempo_total_execucao;
  long tempo_total_ocioso;
  int num_processos_criados;
  int num_preempcoes_total;
  int num_irq[N_IRQ];
  long inicio_tempo_ocioso; // Para calcular o tempo ocioso
} metricas_globais_t;

typedef struct {
  long falhas_pagina_total;
  long transferencias_paginas;
} metricas_vm_t;

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  mmu_t *mmu;
  es_t *es;
  console_t *console;
  bool erro_interno;

  vm_estado_t *vm_estado;

  // Estado dos processos
  processo_t tabela_processos[MAX_PROCESSOS];
  int processo_em_execucao_idx; // Índice na tabela_processos, ou -1 se nenhum
  int proximo_pid;              // Próximo PID a ser alocado

  substituicao_algoritmo_t algoritmo_substituicao;
  int tempo_transferencia_pagina;

  metricas_vm_t metricas_vm;

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

  // Controle para evitar spam quando a CPU permanece em HALT
  bool cpu_em_halt;

  // Controle simplificado do tempo de liberacao da memoria secundaria
  int tempo_disponivel_memsec;
};


// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
static void so_mmu_define_tabpag(so_t *self, tabpag_t *tabpag);
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel, processo_t *destino);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(so_t *self, processo_t *proc, int tam, char str[tam], int ender, bool *bloqueou);
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
static void so_tenta_desbloquear_processo(so_t *self, int idx_proc);
static void so_tenta_desbloquear_leitura(so_t *self, processo_t *proc, int idx_proc);
static void so_tenta_desbloquear_escrita(so_t *self, processo_t *proc, int idx_proc);
static void so_registra_saida_ociosidade(so_t *self);
static void so_registra_entrada_ociosidade(so_t *self);
static processo_t *so_rr_trata_preempcao(so_t *self, processo_t *proc_atual);
static processo_t *so_rr_verifica_processo_atual(so_t *self, processo_t *proc_atual);
static void so_rr_escolhe_novo_processo(so_t *self);
static processo_t *so_prio_trata_processo_atual(so_t *self, processo_t *proc_atual);
static void so_prio_escolhe_melhor(so_t *self);
static int so_prio_encontra_melhor(so_t *self);
static int so_proc_encontra_slot_livre(so_t *self);
static bool so_proc_le_nome_programa(so_t *self, processo_t *criador, char nome[], int tam, bool *bloqueou);
static void so_proc_configura_novo(so_t *self, processo_t *novo_proc, int ender_carga, int idx_tabela, int pid);
static void so_proc_inicializa_vm(so_t *self, processo_t *proc);
static void so_proc_liberacao_recursos(so_t *self, processo_t *proc);
static int so_proc_busca_idx(so_t *self, int pid);
static bool so_proc_desbloqueia_esperando(so_t *self, processo_t *proc_alvo);
static void so_relatorio_atualiza_ociosidade_final(so_t *self, int tempo_final);
static void so_relatorio_imprime_globais(so_t *self, int tempo_final);
static void so_relatorio_imprime_irq(so_t *self);
static void so_relatorio_imprime_processos(so_t *self, int tempo_final);
static void so_relatorio_imprime_processo(so_t *self, processo_t *proc, int tempo_final, const char *estado_nome[]);
static bool so_endereco_valido_para_processo(processo_t *proc, int endereco);
static bool so_atende_falta_pagina(so_t *self, processo_t *proc);
static bool so_vm_salva_quadro(so_t *self, int indice_quadro, int *transferencias);
static bool so_vm_carrega_pagina(so_t *self, processo_t *proc, int pagina_virtual, int indice_quadro, int tempo_carimbo);
static int so_vm_escolhe_quadro_para_carregar(so_t *self);
static int so_vm_agenda_transferencia(so_t *self, int tempo_atual, int transferencias);
static void so_vm_atualiza_idade_quadros(so_t *self);


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

static void so_mmu_define_tabpag(so_t *self, tabpag_t *tabpag)
{
  if (self->mmu == NULL) {
    return;
  }
  mmu_define_tabpag(self->mmu, tabpag);
}


// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, mmu_t *mmu, es_t *es, console_t *console)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->mmu = mmu;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  self->vm_estado = NULL;
  self->algoritmo_substituicao = CONFIG_ALGORITMO_SUBSTITUICAO;
  self->tempo_transferencia_pagina = CONFIG_TEMPO_TRANSFERENCIA_PAGINA;
  self->metricas_vm.falhas_pagina_total = 0;
  self->metricas_vm.transferencias_paginas = 0;
  self->tempo_disponivel_memsec = 0;

  // Inicializa controle de processos
  self->processo_em_execucao_idx = -1;
  self->proximo_pid = 1;
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    self->tabela_processos[i].estado = LIVRE;
    self->tabela_processos[i].tabela_paginas = NULL;
    self->tabela_processos[i].falhas_pagina = 0;
    self->tabela_processos[i].indices_pagsec = NULL;
    self->tabela_processos[i].num_paginas_secundarias = 0;
    self->tabela_processos[i].tamanho_programa = 0;
    self->tabela_processos[i].end_virtual_base = 0;
    self->tabela_processos[i].tempo_desbloqueio = 0;
  }

  // Inicializa fila de prontos (RR)
  self->fila_prontos_inicio = 0;
  self->fila_prontos_fim = 0;
  self->fila_prontos_tamanho = 0;

  // Inicializa escalonador (Padrão: RR com Quantum 3)
  self->escalonador_atual = ESCAL_PRIORIDADE; // Mude para ESCAL_PRIORIDADE para testar o outro
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

  self->cpu_em_halt = false;

  int tam_mem = mem_tam(self->mem);
  int num_quadros = tam_mem / TAM_PAGINA;
  if (num_quadros <= 0) {
    console_printf("SO: Memória física menor que uma página (%d)", tam_mem);
    self->erro_interno = true;
  } else {
    int num_paginas_sec = num_quadros * CONFIG_FATOR_MEM_SECUNDARIA;
    self->vm_estado = vm_estado_cria(num_quadros, num_paginas_sec);
    if (self->vm_estado != NULL) {
      int tam_sec = num_paginas_sec * TAM_PAGINA;
      vm_estado_configura_mem_sec(self->vm_estado, tam_sec);
      int quadros_reservados = (CPU_END_FIM_PROT + 1 + TAM_PAGINA - 1) / TAM_PAGINA;
      int total_quadros = vm_estado_num_quadros(self->vm_estado);
      if (quadros_reservados > total_quadros) {
        quadros_reservados = total_quadros;
      }
      for (int i = 0; i < quadros_reservados; i++) {
        vm_estado_ocupa_quadro(self->vm_estado, i, -1, -1, 0);
      }
    }
  }

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // inicia a MMU em modo físico até despacharmos algum processo
  so_mmu_define_tabpag(self, NULL);

  return self;
}

void so_destroi(so_t *self)
{
  so_mmu_define_tabpag(self, NULL);
  if (self->vm_estado != NULL) {
    for (int i = 0; i < MAX_PROCESSOS; i++) {
      so_proc_liberacao_recursos(self, &self->tabela_processos[i]);
    }
  }
  vm_estado_destroi(self->vm_estado);
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
  bool deve_logar_irq = !(self->cpu_em_halt && irq == IRQ_RELOGIO);
  if (deve_logar_irq) {
    console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  }
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  so_escalona(self);
  // recupera o estado do processo escolhido
  int retorno = so_despacha(self);
  if (self->processo_em_execucao_idx == -1) {
    so_mmu_define_tabpag(self, NULL);
  }
  return retorno;
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
      || mem_le(self->mem, 59, &proc->estado_cpu.regX) != ERR_OK
      || mem_le(self->mem, CPU_END_complemento, &proc->estado_cpu.complemento) != ERR_OK) {
    console_printf("SO: erro na leitura dos registradores para o PCB");
    self->erro_interno = true;
  }
}

static void so_trata_pendencias(so_t *self)
{
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *proc = &self->tabela_processos[i];
    if (proc->estado != BLOQUEADO) {
      continue;
    }
    so_tenta_desbloquear_processo(self, i);
  }
}

static void so_tenta_desbloquear_processo(so_t *self, int idx_proc)
{
  processo_t *proc = &self->tabela_processos[idx_proc];

  switch (proc->motivo_bloqueio) {
    case BLOQUEIO_IO_LE:
      so_tenta_desbloquear_leitura(self, proc, idx_proc);
      break;
    case BLOQUEIO_IO_ESCR:
      so_tenta_desbloquear_escrita(self, proc, idx_proc);
      break;
    case BLOQUEIO_PAGINA: {
      int tempo_atual = so_get_tempo(self);
      if (tempo_atual < proc->tempo_desbloqueio) {
        break;
      }
      console_printf("SO: Processo %d desbloqueado apos transferencia de pagina", proc->pid);
      so_atualiza_estado(self, proc, PRONTO);
      proc->motivo_bloqueio = BLOQUEIO_NENHUM;
      proc->tempo_desbloqueio = 0;
      so_insere_em_pronto(self, idx_proc);
      break;
    }
    default:
      break;
  }
}

static void so_tenta_desbloquear_leitura(so_t *self, processo_t *proc, int idx_proc)
{
  int term = proc->dispositivo_esperado;
  int estado;
  if (es_le(self->es, term + TERM_TECLADO_OK, &estado) != ERR_OK) {
    console_printf("SO (pend): erro ao ler estado teclado (proc %d)", proc->pid);
    so_atualiza_estado(self, proc, LIVRE);
    so_proc_liberacao_recursos(self, proc);
    return;
  }

  if (estado == 0) {
    return;
  }

  int dado;
  if (es_le(self->es, term + TERM_TECLADO, &dado) != ERR_OK) {
    console_printf("SO (pend): erro ao ler teclado (proc %d)", proc->pid);
    so_atualiza_estado(self, proc, LIVRE);
    so_proc_liberacao_recursos(self, proc);
    return;
  }

  proc->estado_cpu.regA = dado;
  so_atualiza_estado(self, proc, PRONTO);
  proc->motivo_bloqueio = BLOQUEIO_NENHUM;
  so_insere_em_pronto(self, idx_proc);
  console_printf("SO: Processo %d desbloqueado por E/S (leitura)", proc->pid);
}

static void so_tenta_desbloquear_escrita(so_t *self, processo_t *proc, int idx_proc)
{
  int term = proc->dispositivo_esperado;
  int estado;
  if (es_le(self->es, term + TERM_TELA_OK, &estado) != ERR_OK) {
    console_printf("SO (pend): erro ao ler estado tela (proc %d)", proc->pid);
    so_atualiza_estado(self, proc, LIVRE);
    so_proc_liberacao_recursos(self, proc);
    return;
  }

  if (estado == 0) {
    return;
  }

  int dado = proc->estado_cpu.regX;
  if (es_escreve(self->es, term + TERM_TELA, dado) != ERR_OK) {
    console_printf("SO (pend): erro ao escrever tela (proc %d)", proc->pid);
    so_atualiza_estado(self, proc, LIVRE);
    so_proc_liberacao_recursos(self, proc);
    return;
  }

  proc->estado_cpu.regA = 0;
  so_atualiza_estado(self, proc, PRONTO);
  proc->motivo_bloqueio = BLOQUEIO_NENHUM;
  so_insere_em_pronto(self, idx_proc);
  console_printf("SO: Processo %d desbloqueado por E/S (escrita)", proc->pid);
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

// Insere processo na estrutura de PRONTOS de acordo com o onador
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
  processo_t *proc_atual = (self->processo_em_execucao_idx != -1)
                             ? &self->tabela_processos[self->processo_em_execucao_idx]
                             : NULL;

  proc_atual = so_rr_trata_preempcao(self, proc_atual);
  proc_atual = so_rr_verifica_processo_atual(self, proc_atual);
  self->deve_preemptar = false;

  if (proc_atual != NULL) {
    return;
  }

  so_rr_escolhe_novo_processo(self);
}

static processo_t *so_rr_trata_preempcao(so_t *self, processo_t *proc_atual)
{
  if (proc_atual == NULL || !self->deve_preemptar) {
    return proc_atual;
  }

  int idx_atual = self->processo_em_execucao_idx;
  console_printf("SO: Preempção RR do processo %d", proc_atual->pid);
  so_registra_preempcao(self, proc_atual);
  so_atualiza_estado(self, proc_atual, PRONTO);
  so_insere_em_pronto(self, idx_atual);
  self->processo_em_execucao_idx = -1;
  return NULL;
}

static processo_t *so_rr_verifica_processo_atual(so_t *self, processo_t *proc_atual)
{
  if (proc_atual == NULL) {
    return NULL;
  }

  if (proc_atual->estado == EXECUTANDO) {
    return proc_atual;
  }

  self->processo_em_execucao_idx = -1;
  return NULL;
}

static void so_rr_escolhe_novo_processo(so_t *self)
{
  int proximo_idx = fila_prontos_remove(self);
  if (proximo_idx == -1) {
    self->processo_em_execucao_idx = -1;
    so_registra_entrada_ociosidade(self);
    return;
  }

  so_registra_saida_ociosidade(self);

  processo_t *proximo_proc = &self->tabela_processos[proximo_idx];
  so_atualiza_estado(self, proximo_proc, EXECUTANDO);
  self->processo_em_execucao_idx = proximo_idx;
  self->quantum_restante = self->quantum_total;
  console_printf("SO: Escalonou %d (RR)", proximo_proc->pid);
}

static void so_registra_saida_ociosidade(so_t *self)
{
  if (self->metricas.inicio_tempo_ocioso == 0) {
    return;
  }

  int tempo_fim = so_get_tempo(self);
  self->metricas.tempo_total_ocioso += (tempo_fim - self->metricas.inicio_tempo_ocioso);
  self->metricas.inicio_tempo_ocioso = 0;
}

static void so_registra_entrada_ociosidade(so_t *self)
{
  if (self->metricas.inicio_tempo_ocioso != 0) {
    return;
  }

  self->metricas.inicio_tempo_ocioso = so_get_tempo(self);
  console_printf("SO: Nenhum processo pronto. Entrando em modo ocioso.");
}

// Escalonador 2: Prioridade
static void so_escalona_prio(so_t *self)
{
  processo_t *proc_atual = (self->processo_em_execucao_idx != -1)
                              ? &self->tabela_processos[self->processo_em_execucao_idx]
                              : NULL;

  proc_atual = so_prio_trata_processo_atual(self, proc_atual);
  self->deve_preemptar = false;

  if (proc_atual != NULL) {
    return;
  }

  so_prio_escolhe_melhor(self);
}

static processo_t *so_prio_trata_processo_atual(so_t *self, processo_t *proc_atual)
{
  if (proc_atual == NULL) {
    return NULL;
  }

  if (proc_atual->estado != EXECUTANDO) {
    if (proc_atual->estado == BLOQUEADO || proc_atual->estado == TERMINADO) {
      so_calcula_prioridade(self, proc_atual);
    }
    self->processo_em_execucao_idx = -1;
    return NULL;
  }

  if (!self->deve_preemptar) {
    return proc_atual;
  }

  so_calcula_prioridade(self, proc_atual);
  so_registra_preempcao(self, proc_atual);
  so_atualiza_estado(self, proc_atual, PRONTO);
  console_printf("SO: Processo %d preemptado por fim de quantum.", proc_atual->pid);
  self->processo_em_execucao_idx = -1;
  return NULL;
}

static void so_prio_escolhe_melhor(so_t *self)
{
  int melhor_idx = so_prio_encontra_melhor(self);

  if (melhor_idx == -1) {
    self->processo_em_execucao_idx = -1;
    so_registra_entrada_ociosidade(self);
    return;
  }

  so_registra_saida_ociosidade(self);

  processo_t *novo_proc = &self->tabela_processos[melhor_idx];
  so_atualiza_estado(self, novo_proc, EXECUTANDO);
  self->processo_em_execucao_idx = melhor_idx;
  self->quantum_restante = self->quantum_total;
  console_printf("SO: Processo %d selecionado para execução (prioridade: %.2f)",
                 novo_proc->pid, novo_proc->prioridade);
}

static int so_prio_encontra_melhor(so_t *self)
{
  int melhor_idx = -1;
  float melhor_prio = 1e9f;

  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *p = &self->tabela_processos[i];
    if (p->estado != PRONTO) {
      continue;
    }
    if (p->prioridade < melhor_prio) {
      melhor_prio = p->prioridade;
      melhor_idx = i;
    }
  }

  return melhor_idx;
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
    if (!self->cpu_em_halt) {
      console_printf("SO: Nenhum processo pronto. CPU em HALT.");
      self->cpu_em_halt = true;
    }
    return 1; // Diz ao trata_int.asm para executar PARA
  }

  // Obtém o ponteiro para o PCB do processo que vai executar
  processo_t *proc = &self->tabela_processos[self->processo_em_execucao_idx];

  self->cpu_em_halt = false;

  // Escreve os registradores do PCB para a memória
  if (mem_escreve(self->mem, CPU_END_A, proc->estado_cpu.regA) != ERR_OK
      || mem_escreve(self->mem, CPU_END_PC, proc->estado_cpu.regPC) != ERR_OK
      || mem_escreve(self->mem, CPU_END_erro, proc->estado_cpu.regERRO) != ERR_OK
      || mem_escreve(self->mem, 59, proc->estado_cpu.regX) != ERR_OK
      || mem_escreve(self->mem, CPU_END_complemento, proc->estado_cpu.complemento) != ERR_OK) {
    console_printf("SO: erro na escrita dos registradores do PCB");
    self->erro_interno = true;
    return 1; // Erro, melhor parar
  }

  // define a tabela de páginas para o processo atual
  so_mmu_define_tabpag(self, proc->tabela_paginas);

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
  int ender = so_carrega_programa(self, "trata_int.maq", NULL);
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupcao");
    self->erro_interno = true;
  }

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // coloca o programa init na memória
  int pid_init = self->proximo_pid;
  processo_t *proc_init = &self->tabela_processos[0];
  so_proc_inicializa_vm(self, proc_init);
  proc_init->pid = pid_init;

  ender = so_carrega_programa(self, "init.maq", proc_init);
  if (ender < 0) {
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }

  // Cria o primeiro processo (init)
  so_inicializa_metricas_processo(self, proc_init, pid_init, ender);
  self->proximo_pid = pid_init + 1;
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
  if (self->processo_em_execucao_idx == -1) {
    // Erro de CPU sem processo (ex: durante o boot, antes do init)
    // Isso é um erro fatal do sistema.
    console_printf("SO: IRQ de erro fatal na CPU (sem processo corrente)!");
    self->erro_interno = true; // Para a simulação
    return;
  }

  processo_t *proc = &self->tabela_processos[self->processo_em_execucao_idx];
  err_t err = proc->estado_cpu.regERRO;

  if (err == ERR_PAG_AUSENTE) {
    if (so_atende_falta_pagina(self, proc)) {
      return;
    }
    console_printf("SO: Processo %d acessou endereco virtual invalido (%d). Processo terminado.",
                   proc->pid, proc->estado_cpu.complemento);
  } else if (err == ERR_END_INV) {
    console_printf("SO: Erro interno: endereco fisico invalido durante traducao (proc %d, complemento %d)",
                   proc->pid, proc->estado_cpu.complemento);
    self->erro_interno = true;
  } else {
    console_printf("SO: Erro na CPU (processo %d): %s. Processo terminado.",
                   proc->pid, err_nome(err));
  }

  so_atualiza_estado(self, proc, LIVRE); // Marca para reutilização
  so_proc_liberacao_recursos(self, proc);
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

  if (self->vm_estado != NULL) {
    so_vm_atualiza_idade_quadros(self);
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
      so_proc_liberacao_recursos(self, proc);
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
    so_proc_liberacao_recursos(self, proc);
    return;
  }

  if (estado != 0) {
    // --- Caminho Rápido (Dispositivo Pronto) ---
    int dado;
    if (es_le(self->es, term + TERM_TECLADO, &dado) != ERR_OK) {
      console_printf("SO: problema no acesso ao teclado (proc %d)", proc->pid);
      self->erro_interno = true;
      so_atualiza_estado(self, proc, LIVRE); // Mata o processo
      so_proc_liberacao_recursos(self, proc);
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
    so_proc_liberacao_recursos(self, proc);
    return;
  }

  if (estado != 0) {
    // --- Caminho Rápido (Dispositivo Pronto) ---
    int dado = proc->estado_cpu.regX;
    if (es_escreve(self->es, term + TERM_TELA, dado) != ERR_OK) {
      console_printf("SO: problema no acesso à tela (proc %d)", proc->pid);
      self->erro_interno = true;
      so_atualiza_estado(self, proc, LIVRE); // Mata o processo
      so_proc_liberacao_recursos(self, proc);
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
  proc->estado_cpu.complemento = 0;
  proc->falhas_pagina = 0;

  // Bloqueio
  proc->motivo_bloqueio = BLOQUEIO_NENHUM;
  proc->pid_esperado = -1;
  proc->dispositivo_esperado = -1;
  proc->tempo_desbloqueio = 0;

  self->metricas.num_processos_criados++;
}

// implementação da chamada se sistema SO_CRIA_PROC
// cria um processo
static void so_chamada_cria_proc(so_t *self)
{
  // Obtém o processo criador
  if (self->processo_em_execucao_idx == -1) return;
  processo_t *criador = &self->tabela_processos[self->processo_em_execucao_idx];

  int novo_idx = so_proc_encontra_slot_livre(self);
  if (novo_idx == -1) {
    console_printf("SO: Limite de processos atingido.");
    criador->estado_cpu.regA = -1; // Retorna erro
    return;
  }

  char nome[100];
  bool bloqueou_mem = false;
  if (!so_proc_le_nome_programa(self, criador, nome, (int)sizeof(nome), &bloqueou_mem)) {
    if (bloqueou_mem) {
      return;
    }
    console_printf("SO: Erro ao ler nome do programa para criar processo.");
    criador->estado_cpu.regA = -1;
    return;
  }

  processo_t *novo_proc = &self->tabela_processos[novo_idx];
  so_proc_inicializa_vm(self, novo_proc);

  int pid_novo = self->proximo_pid;
  novo_proc->pid = pid_novo;

  int ender_carga = so_carrega_programa(self, nome, novo_proc);
  if (ender_carga < 0) {
    console_printf("SO: Erro ao carregar programa '%s'.", nome);
    so_proc_liberacao_recursos(self, novo_proc);
    novo_proc->estado = LIVRE;
    novo_proc->pid = 0;
    criador->estado_cpu.regA = -1;
    return;
  }

  so_proc_configura_novo(self, novo_proc, ender_carga, novo_idx, pid_novo);
  self->proximo_pid = pid_novo + 1;
  so_insere_em_pronto(self, novo_idx);

  criador->estado_cpu.regA = novo_proc->pid;
}

static int so_proc_encontra_slot_livre(so_t *self)
{
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    if (self->tabela_processos[i].estado == LIVRE) {
      return i;
    }
  }
  return -1;
}

static bool so_proc_le_nome_programa(so_t *self, processo_t *criador, char nome[], int tam, bool *bloqueou)
{
  int ender_proc = criador->estado_cpu.regX;
  return copia_str_da_mem(self, criador, tam, nome, ender_proc, bloqueou);
}

static void so_proc_configura_novo(so_t *self, processo_t *novo_proc, int ender_carga, int idx_tabela, int pid)
{
  so_inicializa_metricas_processo(self, novo_proc, pid, ender_carga);
  novo_proc->terminal = D_TERM_A + ((idx_tabela % 4) * 4);
}

static void so_proc_inicializa_vm(so_t *self, processo_t *proc)
{
  if (proc == NULL) {
    return;
  }
  if (proc->tabela_paginas != NULL) {
    tabpag_destroi(proc->tabela_paginas);
  }
  proc->tabela_paginas = tabpag_cria();
  proc->falhas_pagina = 0;
  if (proc->indices_pagsec != NULL) {
    free(proc->indices_pagsec);
  }
  proc->indices_pagsec = NULL;
  proc->num_paginas_secundarias = 0;
  proc->tamanho_programa = 0;
  proc->end_virtual_base = 0;
  proc->tempo_desbloqueio = 0;
}

static void so_proc_liberacao_recursos(so_t *self, processo_t *proc)
{
  if (proc == NULL) {
    return;
  }

  if (proc->tabela_paginas != NULL) {
    tabpag_destroi(proc->tabela_paginas);
    proc->tabela_paginas = NULL;
  }

  if (self->vm_estado != NULL) {
    int num_quadros = vm_estado_num_quadros(self->vm_estado);
    for (int i = 0; i < num_quadros; i++) {
      quadro_desc_t *q = vm_estado_quadro(self->vm_estado, i);
      if (q == NULL) {
        continue;
      }
      if (!q->livre && q->dono_pid == proc->pid) {
        vm_estado_libera_quadro(self->vm_estado, i);
      }
    }

    int num_paginas_sec = vm_estado_num_paginas_sec(self->vm_estado);
    for (int i = 0; i < num_paginas_sec; i++) {
      pagina_sec_desc_t *p = vm_estado_pagina_sec(self->vm_estado, i);
      if (p == NULL) {
        continue;
      }
      if (p->ocupado && p->dono_pid == proc->pid) {
        vm_estado_libera_pagsec(self->vm_estado, i);
      }
    }
  }

  if (proc->indices_pagsec != NULL) {
    free(proc->indices_pagsec);
    proc->indices_pagsec = NULL;
  }
  proc->tamanho_programa = 0;
  proc->end_virtual_base = 0;
  proc->tempo_desbloqueio = 0;
}

static int so_proc_busca_idx(so_t *self, int pid)
{
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *proc = &self->tabela_processos[i];
    if (proc->pid == pid && proc->estado != LIVRE) {
      return i;
    }
  }
  return -1;
}

static bool so_proc_desbloqueia_esperando(so_t *self, processo_t *proc_alvo)
{
  bool coletado = false;
  int pid_alvo = proc_alvo->pid;

  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *proc_esperando = &self->tabela_processos[i];
    if (proc_esperando->estado != BLOQUEADO) {
      continue;
    }
    if (proc_esperando->motivo_bloqueio != BLOQUEIO_PID) {
      continue;
    }
    if (proc_esperando->pid_esperado != pid_alvo) {
      continue;
    }

    console_printf("SO: Desbloqueando processo %d (esperava por %d).",
                   proc_esperando->pid, pid_alvo);
    so_atualiza_estado(self, proc_esperando, PRONTO);
    proc_esperando->motivo_bloqueio = BLOQUEIO_NENHUM;
    proc_esperando->estado_cpu.regA = 0;
    so_insere_em_pronto(self, i);

    so_atualiza_estado(self, proc_alvo, LIVRE);
    so_proc_liberacao_recursos(self, proc_alvo);
    coletado = true;
    break;
  }

  return coletado;
}

// ---------------------------------------------------------------------
// MEMÓRIA VIRTUAL {{{1
// ---------------------------------------------------------------------

static bool so_endereco_valido_para_processo(processo_t *proc, int endereco)
{
  if (proc == NULL) {
    return false;
  }
  if (proc->indices_pagsec == NULL) {
    return false;
  }
  if (proc->num_paginas_secundarias <= 0) {
    return false;
  }

  int base = proc->end_virtual_base;
  int limite = base + proc->num_paginas_secundarias * TAM_PAGINA;
  if (endereco < base) {
    return false;
  }
  if (endereco >= limite) {
    return false;
  }
  return true;
}

static int so_vm_agenda_transferencia(so_t *self, int tempo_atual, int transferencias)
{
  if (transferencias <= 0) {
    return tempo_atual;
  }

  int inicio = tempo_atual;
  if (self->tempo_disponivel_memsec > inicio) {
    inicio = self->tempo_disponivel_memsec;
  }

  int fim = inicio + transferencias * self->tempo_transferencia_pagina;
  self->tempo_disponivel_memsec = fim;
  self->metricas_vm.transferencias_paginas += transferencias;
  return fim;
}

static int so_vm_escolhe_quadro_para_carregar(so_t *self)
{
  if (self == NULL || self->vm_estado == NULL) {
    return -1;
  }

  int num_quadros = vm_estado_num_quadros(self->vm_estado);

  for (int i = 0; i < num_quadros; i++) {
    quadro_desc_t *quadro = vm_estado_quadro(self->vm_estado, i);
    if (quadro == NULL) {
      continue;
    }
    if (quadro->livre) {
      return i;
    }
  }

  if (self->algoritmo_substituicao == SUBSTITUICAO_FIFO) {
    int escolhido = -1;
    unsigned long melhor_carimbo = 0;
    bool inicializado = false;

    for (int i = 0; i < num_quadros; i++) {
      quadro_desc_t *quadro = vm_estado_quadro(self->vm_estado, i);
      if (quadro == NULL || quadro->livre || quadro->dono_pid < 0) {
        continue;
      }
      if (!inicializado || quadro->carimbo_fifo < melhor_carimbo) {
        melhor_carimbo = quadro->carimbo_fifo;
        escolhido = i;
        inicializado = true;
      }
    }

    return escolhido;
  }

  int escolhido = -1;
  unsigned long melhor_idade = 0;
  bool inicializado = false;

  for (int i = 0; i < num_quadros; i++) {
    quadro_desc_t *quadro = vm_estado_quadro(self->vm_estado, i);
    if (quadro == NULL || quadro->livre || quadro->dono_pid < 0) {
      continue;
    }
    if (!inicializado || quadro->idade < melhor_idade) {
      melhor_idade = quadro->idade;
      escolhido = i;
      inicializado = true;
    }
  }

  return escolhido;
}

static bool so_vm_salva_quadro(so_t *self, int indice_quadro, int *transferencias)
{
  if (self == NULL || self->vm_estado == NULL) {
    return false;
  }

  quadro_desc_t *quadro = vm_estado_quadro(self->vm_estado, indice_quadro);
  if (quadro == NULL) {
    return false;
  }

  if (quadro->livre) {
    vm_estado_libera_quadro(self->vm_estado, indice_quadro);
    return true;
  }

  if (quadro->dono_pid < 0 || quadro->pagina_virtual < 0) {
    return false;
  }

  int pid_dono = quadro->dono_pid;
  int pagina_virtual = quadro->pagina_virtual;
  int idx_proc = so_proc_busca_idx(self, pid_dono);
  processo_t *proc_dono = idx_proc >= 0 ? &self->tabela_processos[idx_proc] : NULL;

  bool precisa_gravar = false;
  int slot_secundario = -1;

  if (proc_dono != NULL && proc_dono->tabela_paginas != NULL && pagina_virtual >= 0) {
    precisa_gravar = tabpag_bit_alteracao(proc_dono->tabela_paginas, pagina_virtual);
    tabpag_invalida_pagina(proc_dono->tabela_paginas, pagina_virtual);

    if (proc_dono->indices_pagsec != NULL && pagina_virtual < proc_dono->num_paginas_secundarias) {
      slot_secundario = proc_dono->indices_pagsec[pagina_virtual];
    }
  }

  if (precisa_gravar && slot_secundario < 0) {
    return false;
  }

  if (precisa_gravar) {
    int base_sec = slot_secundario * TAM_PAGINA;
    int base_fis = indice_quadro * TAM_PAGINA;

    for (int offset = 0; offset < TAM_PAGINA; offset++) {
      int valor = 0;
      if (mem_le(self->mem, base_fis + offset, &valor) != ERR_OK) {
        return false;
      }
      if (vm_estado_sec_escreve(self->vm_estado, base_sec + offset, valor) != ERR_OK) {
        return false;
      }
    }

    if (transferencias != NULL) {
      (*transferencias)++;
    }
  }

  vm_estado_libera_quadro(self->vm_estado, indice_quadro);
  return true;
}

static bool so_vm_carrega_pagina(so_t *self, processo_t *proc, int pagina_virtual, int indice_quadro, int tempo_carimbo)
{
  if (self == NULL || self->vm_estado == NULL || proc == NULL) {
    return false;
  }
  if (proc->indices_pagsec == NULL) {
    return false;
  }
  if (pagina_virtual < 0 || pagina_virtual >= proc->num_paginas_secundarias) {
    return false;
  }

  int slot_secundario = proc->indices_pagsec[pagina_virtual];
  if (slot_secundario < 0) {
    return false;
  }

  int base_sec = slot_secundario * TAM_PAGINA;
  int base_fis = indice_quadro * TAM_PAGINA;

  for (int offset = 0; offset < TAM_PAGINA; offset++) {
    int valor = 0;
    if (vm_estado_sec_le(self->vm_estado, base_sec + offset, &valor) != ERR_OK) {
      return false;
    }
    if (mem_escreve(self->mem, base_fis + offset, valor) != ERR_OK) {
      return false;
    }
  }

  vm_estado_ocupa_quadro(self->vm_estado, indice_quadro, proc->pid, pagina_virtual, (unsigned long)tempo_carimbo);

  quadro_desc_t *quadro = vm_estado_quadro(self->vm_estado, indice_quadro);
  if (quadro != NULL) {
    quadro->idade = LRU_MSB_MASK;
  }

  if (proc->tabela_paginas == NULL) {
    return false;
  }
  tabpag_define_quadro(proc->tabela_paginas, pagina_virtual, indice_quadro);
  return true;
}

static bool so_atende_falta_pagina(so_t *self, processo_t *proc)
{
  if (self == NULL || proc == NULL || self->vm_estado == NULL) {
    return false;
  }

  int endereco = proc->estado_cpu.complemento;
  if (!so_endereco_valido_para_processo(proc, endereco)) {
    return false;
  }

  int pagina_virtual = (endereco - proc->end_virtual_base) / TAM_PAGINA;
  int tempo_atual = so_get_tempo(self);
  int transferencias = 1; // leitura obrigatória da página faltante

  int indice_quadro = vm_estado_busca_quadro_livre(self->vm_estado);
  if (indice_quadro < 0) {
    indice_quadro = so_vm_escolhe_quadro_para_carregar(self);
    if (indice_quadro < 0) {
      return false;
    }
    if (!so_vm_salva_quadro(self, indice_quadro, &transferencias)) {
      int indice_alternativo = so_vm_escolhe_quadro_para_carregar(self);
      if (indice_alternativo < 0) {
        return false;
      }
      indice_quadro = indice_alternativo;
      if (!so_vm_salva_quadro(self, indice_quadro, &transferencias)) {
        return false;
      }
    }
  }

  if (!so_vm_carrega_pagina(self, proc, pagina_virtual, indice_quadro, tempo_atual)) {
    return false;
  }

  int tempo_desbloqueio = so_vm_agenda_transferencia(self, tempo_atual, transferencias);

  proc->falhas_pagina++;
  self->metricas_vm.falhas_pagina_total++;
  proc->estado_cpu.regERRO = ERR_OK;
  proc->motivo_bloqueio = BLOQUEIO_PAGINA;
  proc->tempo_desbloqueio = tempo_desbloqueio;
  proc->pid_esperado = -1;
  proc->dispositivo_esperado = -1;

  so_atualiza_estado(self, proc, BLOQUEADO);

  console_printf("SO: Falta de pagina atendida (proc %d, pagina %d -> quadro %d)",
                 proc->pid, pagina_virtual, indice_quadro);

  return true;
}

static void so_vm_atualiza_idade_quadros(so_t *self)
{
  if (self == NULL || self->vm_estado == NULL) {
    return;
  }

  int num_quadros = vm_estado_num_quadros(self->vm_estado);
  for (int i = 0; i < num_quadros; i++) {
    quadro_desc_t *quadro = vm_estado_quadro(self->vm_estado, i);
    if (quadro == NULL || quadro->livre) {
      continue;
    }

    quadro->idade >>= 1;

    bool acessada = false;
    if (quadro->dono_pid > 0 && quadro->pagina_virtual >= 0) {
      int idx_proc = so_proc_busca_idx(self, quadro->dono_pid);
      if (idx_proc >= 0) {
        processo_t *proc = &self->tabela_processos[idx_proc];
        if (proc->tabela_paginas != NULL) {
          acessada = tabpag_bit_acesso(proc->tabela_paginas, quadro->pagina_virtual);
          tabpag_zera_bit_acesso(proc->tabela_paginas, quadro->pagina_virtual);
        }
      }
    }

    if (acessada) {
      quadro->idade |= LRU_MSB_MASK;
    }
  }
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

  int idx_alvo = so_proc_busca_idx(self, pid_alvo);
  if (idx_alvo == -1) {
    console_printf("SO: Tentativa de matar processo inexistente ou já morto (PID %d)", pid_alvo);
    chamador->estado_cpu.regA = -1; // Erro
    return;
  }

  processo_t *proc_alvo = &self->tabela_processos[idx_alvo];
  if (proc_alvo->estado == LIVRE || proc_alvo->estado == TERMINADO) {
    console_printf("SO: Tentativa de matar processo inexistente ou já morto (PID %d)", pid_alvo);
    chamador->estado_cpu.regA = -1;
    return;
  }

  // Muda o estado do processo alvo para TERMINADO
  so_atualiza_estado(self, proc_alvo, TERMINADO);
  so_proc_liberacao_recursos(self, proc_alvo);
  console_printf("SO: Processo %d terminado.", pid_alvo);

  bool coletado = so_proc_desbloqueia_esperando(self, proc_alvo);
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

  int idx_alvo = so_proc_busca_idx(self, pid_alvo);
  if (idx_alvo == -1) {
    console_printf("SO: Processo %d tentou esperar por PID inexistente %d.", chamador->pid, pid_alvo);
    chamador->estado_cpu.regA = -1; // Erro
    return;
  }

  processo_t *proc_alvo = &self->tabela_processos[idx_alvo];
  if (proc_alvo->estado == TERMINADO) {
    console_printf("SO: Processo %d esperou por PID %d (já terminado). Coletando.",
                   chamador->pid, pid_alvo);
    so_atualiza_estado(self, proc_alvo, LIVRE);
    so_proc_liberacao_recursos(self, proc_alvo);
    chamador->estado_cpu.regA = 0;
    return;
  }

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
// - se destino == NULL, carrega diretamente na memória física (uso interno: BIOS, tratadores)
// - caso contrário, grava o conteúdo na memória secundária e registra metadados no processo
// retorna o endereço virtual inicial ou -1 em erro
static int so_carrega_programa(so_t *self, char *nome_do_executavel, processo_t *destino)
{
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_ini = prog_end_carga(prog);
  int tam_prog = prog_tamanho(prog);
  int end_fim = end_ini + tam_prog;

  if (destino == NULL) {
    for (int end = end_ini; end < end_fim; end++) {
      if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK) {
        console_printf("Erro na carga da memória, endereco %d\n", end);
        prog_destroi(prog);
        return -1;
      }
    }
    prog_destroi(prog);
    console_printf("SO: carga fisica de '%s' em %d-%d", nome_do_executavel, end_ini, end_fim);
    return end_ini;
  }

  if (self->vm_estado == NULL) {
    console_printf("SO: memória secundária indisponível para carregar '%s'", nome_do_executavel);
    prog_destroi(prog);
    return -1;
  }

  int num_paginas = (tam_prog + TAM_PAGINA - 1) / TAM_PAGINA;
  if (num_paginas <= 0) {
    num_paginas = 1;
  }

  int *indices = malloc(sizeof(int) * num_paginas);
  if (indices == NULL) {
    console_printf("SO: falta de memória ao preparar carga de '%s'", nome_do_executavel);
    prog_destroi(prog);
    return -1;
  }
  for (int i = 0; i < num_paginas; i++) {
    indices[i] = -1;
  }

  for (int pagina = 0; pagina < num_paginas; pagina++) {
    int slot = vm_estado_busca_pagsec_livre(self->vm_estado);
    if (slot < 0) {
      console_printf("SO: memória secundária insuficiente para '%s'", nome_do_executavel);
      for (int j = 0; j < pagina; j++) {
        if (indices[j] >= 0) {
          vm_estado_libera_pagsec(self->vm_estado, indices[j]);
        }
      }
      free(indices);
      prog_destroi(prog);
      return -1;
    }

    indices[pagina] = slot;
    int base = slot * TAM_PAGINA;
    int deslocamento_pagina = pagina * TAM_PAGINA;
    int restante = tam_prog - deslocamento_pagina;
    int tamanho_util = TAM_PAGINA;
    if (restante > 0 && restante < TAM_PAGINA) {
      tamanho_util = restante;
    }
    vm_estado_ocupa_pagsec(self->vm_estado, slot, destino->pid, pagina, base, tamanho_util);

    for (int offset = 0; offset < TAM_PAGINA; offset++) {
      int pos_prog = deslocamento_pagina + offset;
      int valor = 0;
      if (pos_prog < tam_prog) {
        valor = prog_dado(prog, end_ini + pos_prog);
      }
      if (vm_estado_sec_escreve(self->vm_estado, base + offset, valor) != ERR_OK) {
        console_printf("SO: erro ao escrever memória secundária (%s)", nome_do_executavel);
        for (int j = 0; j <= pagina; j++) {
          if (indices[j] >= 0) {
            vm_estado_libera_pagsec(self->vm_estado, indices[j]);
          }
        }
        free(indices);
        prog_destroi(prog);
        return -1;
      }
    }
  }

  if (destino->indices_pagsec != NULL) {
    free(destino->indices_pagsec);
  }
  destino->indices_pagsec = indices;
  destino->num_paginas_secundarias = num_paginas;
  destino->tamanho_programa = tam_prog;
  destino->end_virtual_base = end_ini;

  prog_destroi(prog);
  console_printf("SO: carga de '%s' em memoria secundaria (%d paginas)", nome_do_executavel, num_paginas);
  return end_ini;
}


// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

// copia uma string da memória do simulador para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// t2: deveria verificar se a memória pertence ao processo
static bool copia_str_da_mem(so_t *self, processo_t *proc, int tam, char str[tam], int ender, bool *bloqueou)
{
  if (bloqueou != NULL) {
    *bloqueou = false;
  }
  if (self == NULL || proc == NULL || self->mmu == NULL) {
    return false;
  }

  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere = 0;
    err_t err = mmu_le(self->mmu, ender + indice_str, &caractere, usuario);
    if (err == ERR_OK) {
      if (caractere < 0 || caractere > 255) {
        return false;
      }
      str[indice_str] = caractere;
      if (caractere == 0) {
        return true;
      }
      continue;
    }

    if (err == ERR_PAG_AUSENTE) {
      proc->estado_cpu.complemento = ender + indice_str;
      if (so_atende_falta_pagina(self, proc)) {
        if (bloqueou != NULL) {
          *bloqueou = true;
        }
        if (proc->estado_cpu.regPC > 0) {
          proc->estado_cpu.regPC -= 1;
        }
      }
    } else if (err == ERR_END_INV) {
      proc->estado_cpu.complemento = ender + indice_str;
    }
    return false;
  }

  // estourou o tamanho de str
  return false;
}

// Imprime o relatório final de métricas do sistema
static void so_imprime_relatorio_final(so_t *self)
{
  int tempo_final = so_get_tempo(self);
  so_relatorio_atualiza_ociosidade_final(self, tempo_final);

  console_printf("\n=== Relatório Final do Sistema ===");
  so_relatorio_imprime_globais(self, tempo_final);
  so_relatorio_imprime_irq(self);
  so_relatorio_imprime_processos(self, tempo_final);
  console_printf("=== Fim do Relatório ===\n");
}

static void so_relatorio_atualiza_ociosidade_final(so_t *self, int tempo_final)
{
  self->metricas.tempo_total_execucao = tempo_final;
  if (self->metricas.inicio_tempo_ocioso == 0) {
    return;
  }

  self->metricas.tempo_total_ocioso += (tempo_final - self->metricas.inicio_tempo_ocioso);
  self->metricas.inicio_tempo_ocioso = 0;
}

static void so_relatorio_imprime_globais(so_t *self, int tempo_final)
{
  console_printf("Processos criados: %d", self->metricas.num_processos_criados);
  console_printf("Tempo total: %ld ticks", self->metricas.tempo_total_execucao);

  float percentual_ocioso = 0.0f;
  if (tempo_final > 0) {
    percentual_ocioso = 100.0f * (float)self->metricas.tempo_total_ocioso / tempo_final;
  }

  console_printf("Tempo ocioso: %ld ticks (%.1f%%)",
                 self->metricas.tempo_total_ocioso, percentual_ocioso);
  console_printf("Preempções totais: %d", self->metricas.num_preempcoes_total);
  console_printf("Falhas de página (total): %ld", self->metricas_vm.falhas_pagina_total);
  console_printf("Transferências de página: %ld", self->metricas_vm.transferencias_paginas);
}

static void so_relatorio_imprime_irq(so_t *self)
{
  console_printf("\nInterrupções por tipo:");
  for (int i = 0; i < N_IRQ; i++) {
    console_printf("  IRQ %-2d (%-12s): %d", i, irq_nome(i), self->metricas.num_irq[i]);
  }
}

static void so_relatorio_imprime_processos(so_t *self, int tempo_final)
{
  static const char *estado_nome[] = {
    "LIVRE", "PRONTO", "EXECUTANDO", "BLOQUEADO", "TERMINADO"
  };

  console_printf("\nProcessos:");
  for (int i = 0; i < MAX_PROCESSOS; i++) {
    processo_t *proc = &self->tabela_processos[i];
    if (proc->pid == 0) {
      continue;
    }
    so_relatorio_imprime_processo(self, proc, tempo_final, estado_nome);
  }
}

static void so_relatorio_imprime_processo(so_t *self, processo_t *proc, int tempo_final, const char *estado_nome[])
{
  int tempo_termino = (proc->tempo_termino >= 0) ? proc->tempo_termino : tempo_final;
  int tempo_retorno = tempo_termino - proc->tempo_criacao;
  if (tempo_retorno < 0) {
    tempo_retorno = 0;
  }

  int tempos_estado[5];
  for (int e = 0; e < 5; e++) {
    tempos_estado[e] = proc->tempo_total_estado[e];
  }

  if (proc->estado != LIVRE && proc->estado != TERMINADO) {
    int tempo_atual = tempo_final - proc->ultimo_tempo_mudanca_estado;
    if (tempo_atual > 0 && proc->estado >= 0 && proc->estado < 5) {
      tempos_estado[proc->estado] += tempo_atual;
    }
  }

  float tempo_resposta = 0.0f;
  int execucoes = proc->contagem_estado[EXECUTANDO];
  if (execucoes > 0) {
    tempo_resposta = (float)proc->tempo_total_pronto / execucoes;
  }

  console_printf("\n  PID %-3d retorno=%d preemp=%d",
                 proc->pid, tempo_retorno, proc->num_preempcoes);
  console_printf("    estados:");
  for (int e = 0; e < 5; e++) {
    console_printf("      %-10s entradas=%-3d tempo=%d",
                   estado_nome[e], proc->contagem_estado[e], tempos_estado[e]);
  }
  console_printf("    resposta média: %.2f ticks", tempo_resposta);
  console_printf("    memória virtual: faltas=%d páginas_sec=%d\n",
                 proc->falhas_pagina, proc->num_paginas_secundarias);
}

// vim: foldmethod=marker
