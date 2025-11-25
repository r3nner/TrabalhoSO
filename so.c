// so.c
// sistema operacional
// simulador de computador
// so25b

// ---------------------------------------------------------------------
// INCLUDES {{{1
// ---------------------------------------------------------------------

#include "so.h"
#include "cpu.h"
#include "dispositivos.h"
#include "err.h"
#include "irq.h"
#include "memoria.h"
#include "programa.h"
#include "tabpag.h"

#include <stdlib.h>
#include <stdbool.h>


// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas

// Não tem processos nem memória virtual, mas é preciso usar a paginação,
//   pelo menos para implementar relocação, já que os programas estão sendo
//   todos montados para serem executados no endereço 0 e o endereço 0
//   físico é usado pelo hardware nas interrupções.
// Os programas estão sendo carregados no início de um quadro, e usam quantos
//   quadros forem necessárias. Para isso a variável quadro_livre contém
//   o número do primeiro quadro da memória principal que ainda não foi usado.
//   Na carga do processo, a tabela de páginas (deveria ter uma por processo,
//   mas não tem processo) é alterada para que o endereço virtual 0 resulte
//   no quadro onde o programa foi carregado. Com isso, o programa carregado
//   é acessível, mas o acesso ao anterior é perdido.

// t3: a interface de algumas funções que manipulam memória teve que ser alterada,
//   para incluir o processo ao qual elas se referem. Para isso, é necessário um
//   tipo de dados para identificar um processo. Neste código, não tem processos
//   implementados, e não tem um tipo para isso. Foi usado o tipo int.
//   É necessário também um valor para representar um processo inexistente.
//   Foi usado o valor -1. Altere para o seu tipo, ou substitua os usos de
//   processo_t e NENHUM_PROCESSO para o seu tipo.
//   ALGUM_PROCESSO serve para representar um processo que não é NENHUM. Só tem
//   algum sentido enquanto não tem implementação de processos.

#include <string.h>
#include "quadros.h"


// Funções auxiliares para lista de processos prontos
typedef struct proc_node_t {
    processo_t *proc;
    struct proc_node_t *prox;
} proc_node_t;

void fila_push(proc_node_t **fila, processo_t *proc) {
    proc_node_t *novo = malloc(sizeof(proc_node_t));
    novo->proc = proc;
    novo->prox = NULL;
    if (!*fila) {
        *fila = novo;
    } else {
        proc_node_t *atual = *fila;
        while (atual->prox) atual = atual->prox;
        atual->prox = novo;
    }
}

processo_t *fila_pop(proc_node_t **fila) {
    if (!*fila) return NULL;
    proc_node_t *rem = *fila;
    processo_t *proc = rem->proc;
    *fila = rem->prox;
    free(rem);
    return proc;
}

// so_t is defined in so.h; here we implement helpers for processes

processo_t *processo_cria(int pid, int tam_mem_virtual) {
  processo_t *proc = malloc(sizeof(processo_t));
  if (!proc) return NULL;
  proc->pid = pid;
  proc->tabpag = tabpag_cria();
  proc->tam_mem_virtual = tam_mem_virtual;
  proc->sec_base = -1;
  proc->page_faults = 0;
  proc->blocked_until = 0;
  proc->next = NULL;
  return proc;
}

void processo_destroi(processo_t *proc) {
  if (!proc) return;
  if (proc->tabpag) tabpag_destroi(proc->tabpag);
  free(proc);
}

// registra um processo na lista do SO
static void so_registra_processo(so_t *self, processo_t *proc) {
  if (!self || !proc) return;
  proc->next = self->proc_list;
  self->proc_list = proc;
}

// procura processo pelo pid
static processo_t *so_proc_pelo_pid(so_t *self, int pid) {
  if (!self) return NULL;
  processo_t *p = self->proc_list;
  while (p) {
    if (p->pid == pid) return p;
    p = p->next;
  }
  return NULL;
}


// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// carrega o programa contido no arquivo para memória virtual de um processo
// retorna o endereço virtual inicial de execução
static int so_carrega_programa(so_t *self, processo_t *processo, char *nome_do_executavel);
// forward declarations for specific loaders (used before their later definitions)
static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa);
static int so_carrega_programa_na_memoria_virtual(so_t *self, programa_t *programa, processo_t *processo);
// copia para str da memória do processo, até copiar um 0 (retorna true) ou tam bytes
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam], int end_virt, processo_t *processo);

// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, mem_t *mem_secundaria, mmu_t *mmu,
              es_t *es, console_t *console)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->mem_secundaria = mem_secundaria;
  self->mmu = mmu;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  self->proc_list = NULL;
  self->quadros = quadros_cria(self->mem);
  self->usar_lru = 1; // habilita LRU por padrão
  // disco: quando estará livre (em instruções)
  self->disco_livre_em = 0;
  // tempo (em instruções) para transferir UMA página entre mem e mem_sec
  self->tempo_transferencia_pagina = 50; // valor default, pode ser ajustado

  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);
  // Não há tabela de páginas global, cada processo terá a sua
  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  // destroi gerenciador de quadros se existir
  if (self->quadros) quadros_destroi(self->quadros);
  // destroi processos registrados
  processo_t *p = self->proc_list;
  while (p) {
    processo_t *nx = p->next;
    processo_destroi(p);
    p = nx;
  }
  free(self);
}


// ---------------------------------------------------------------------
// TRATAMENTO DE INTERRUPÇÃO {{{1
// ---------------------------------------------------------------------

// funções auxiliares para o tratamento de interrupção
static void so_salva_estado_da_cpu(so_t *self);
static void so_trata_irq(so_t *self, int irq);
static void so_trata_pendencias(so_t *self);
static void so_escalona(so_t *self);
static int so_despacha(so_t *self);

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
  // t2: salva os registradores que compõem o estado da cpu no descritor do
  //   processo corrente. os valores dos registradores foram colocados pela
  //   CPU na memória, nos endereços CPU_END_PC etc. O registrador X foi salvo
  //   pelo tratador de interrupção (ver trata_irq.asm) no endereço 59
  // se não houver processo corrente, não faz nada
  if (mem_le(self->mem, CPU_END_A, &self->regA) != ERR_OK
      || mem_le(self->mem, CPU_END_PC, &self->regPC) != ERR_OK
      || mem_le(self->mem, CPU_END_erro, &self->regERRO) != ERR_OK
      || mem_le(self->mem, CPU_END_complemento, &self->regComplemento) != ERR_OK
      || mem_le(self->mem, 59, &self->regX)) {
    console_printf("SO: erro na leitura dos registradores");
    self->erro_interno = true;
  }
}

static void so_trata_pendencias(so_t *self)
{
  // trata pendências gerais: E/S pendente, desbloqueio de processos, contabilidades
  // aqui tratamos desbloqueio de processos bloqueados por transferência de páginas
  int now = 0;
  if (es_le(self->es, D_RELOGIO_INSTRUCOES, &now) != ERR_OK) {
    console_printf("SO: erro ao ler relogio em so_trata_pendencias");
    self->erro_interno = true;
    return;
  }
  // percorre lista de processos e desbloqueia os que expiraram
  processo_t *p = self->proc_list;
  while (p) {
    if (p->blocked_until > 0 && p->blocked_until <= now) {
      console_printf("SO: desbloqueando PID %d (agora=%d)", p->pid, now);
      p->blocked_until = 0;
      // se não há processo corrente, escalar este processo como corrente
      if (self->proc_corrente == NULL) {
        self->proc_corrente = p;
      }
    }
    p = p->next;
  }
}

static void so_escalona(so_t *self)
{
  // Por enquanto, só existe o processo init
  // Futuramente, aqui será feita a escolha do próximo processo pronto
  // Se houver troca de processo, atualize a MMU:
  if (self->proc_corrente) {
    mmu_define_tabpag(self->mmu, self->proc_corrente->tabpag);
  }
}

static int so_despacha(so_t *self)
{
  // t2: se houver processo corrente, coloca o estado desse processo onde ele
  //   será recuperado pela CPU (em CPU_END_PC etc e 59) e retorna 0,
  //   senão retorna 1
  // o valor retornado será o valor de retorno de CHAMAC, e será colocado no 
  //   registrador A para o tratador de interrupção (ver trata_irq.asm).
  if (mem_escreve(self->mem, CPU_END_A, self->regA) != ERR_OK
      || mem_escreve(self->mem, CPU_END_PC, self->regPC) != ERR_OK
      || mem_escreve(self->mem, CPU_END_erro, self->regERRO) != ERR_OK
      || mem_escreve(self->mem, 59, self->regX)) {
    console_printf("SO: erro na escrita dos registradores");
    self->erro_interno = true;
  }
  if (self->erro_interno) return 1;
  else return 0;
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
  // Coloca o tratador de interrupção na memória física
  int ender = so_carrega_programa(self, NULL, "trata_int.maq");
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // Programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // Define o primeiro quadro livre de memória como o seguinte àquele que
  // contém o endereço final da memória protegida
  self->quadro_livre = CPU_END_FIM_PROT / TAM_PAGINA + 1;

  // Cria o processo init (PID 1) e carrega o programa na memória secundária
  int pid_init = 1;
  int tam_virtual = 4096; // exemplo: 4096 palavras de espaço virtual
  self->proc_corrente = processo_cria(pid_init, tam_virtual);
  if (!self->proc_corrente) {
    console_printf("SO: erro ao criar processo init");
    self->erro_interno = true;
    return;
  }
  // registra processo na lista do SO
  so_registra_processo(self, self->proc_corrente);
  programa_t *prog = prog_cria("init.maq");
  if (!prog) {
    console_printf("SO: erro ao carregar programa init.maq");
    self->erro_interno = true;
    return;
  }
  int ender_init = so_carrega_programa_na_memoria_virtual(self, prog, self->proc_corrente);
  prog_destroi(prog);
  if (ender_init == -1) {
    console_printf("SO: problema na carga do programa inicial");
    self->erro_interno = true;
    return;
  }
  // Altera o PC para o endereço de carga do processo init
  self->regPC = ender_init;
  // Configura a MMU para usar a tabela de páginas do processo corrente
  mmu_define_tabpag(self->mmu, self->proc_corrente->tabpag);
}

// interrupção gerada quando a CPU identifica um erro
static void so_trata_irq_err_cpu(so_t *self)
{
  // Ocorreu um erro interno na CPU
  err_t err = self->regERRO;
  int endereco_faltante = self->regComplemento; // endereço virtual que causou a falta
  if (err == ERR_PAG_AUSENTE) {
    // Falta de página: verificar se o acesso é válido para o processo
    console_printf("SO: Falta de página no endereco virtual %d (PID %d)", endereco_faltante, self->proc_corrente ? self->proc_corrente->pid : -1);
    if (!self->proc_corrente) {
      console_printf("SO: sem processo corrente ao tratar falta de pagina");
      self->erro_interno = true;
      return;
    }
    processo_t *proc = self->proc_corrente;
    int pagina = endereco_faltante / TAM_PAGINA;
    int max_paginas = (proc->tam_mem_virtual + TAM_PAGINA - 1) / TAM_PAGINA;
    if (pagina < 0 || pagina >= max_paginas) {
      // endereço inválido para o processo: matar (por enquanto, marca erro)
      console_printf("SO: endereco virtual %d invalido para PID %d -- kill", endereco_faltante, proc->pid);
      // TODO: Implementar remoção correta do processo
      self->erro_interno = true;
      return;
    }

  // tenta encontrar quadro livre
  int quadro = quadros_encontra_livre(self->quadros);
  int vitima = -1;
  bool need_writeback = false;
    if (quadro == -1) {
      // escolhe vitima: LRU ou FIFO
      if (self->usar_lru) {
        // define callback that looks up process by pid and asks its tabpag for age
        bool get_age_cb(int pid, int pagina, unsigned *page_age, void *user_data) {
          so_t *so = user_data;
          processo_t *p = so_proc_pelo_pid(so, pid);
          if (!p) return false;
          return tabpag_get_age(p->tabpag, pagina, page_age);
        }
        vitima = quadros_seleciona_vitima_lru(self->quadros, get_age_cb, self);
      } else {
        vitima = quadros_seleciona_vitima(self->quadros);
      }
      if (vitima == -1) {
        console_printf("SO: nao ha quadro para substituir (nenhuma pagina alocada)");
        self->erro_interno = true;
        return;
      }
      // remove da fila
      int removed = quadros_remove_vitima(self->quadros);
      if (removed != vitima) {
        // inconsistencia, mas prossegue
      }
      quadro = vitima;
      // obter dono da pagina vitima
      int dono_pid = quadros_owner_pid(self->quadros, quadro);
      int dono_pagina = quadros_owner_pagina(self->quadros, quadro);
      processo_t *dono = so_proc_pelo_pid(self, dono_pid);
      if (dono) {
        // se alterada, escrever de volta na memoria secundaria
        if (tabpag_bit_alteracao(dono->tabpag, dono_pagina)) {
          need_writeback = true;
          for (int off = 0; off < TAM_PAGINA; off++) {
            int val;
            if (mem_le(self->mem, quadro * TAM_PAGINA + off, &val) != ERR_OK) {
              console_printf("SO: erro ao ler quadro %d para escrita de volta", quadro);
              self->erro_interno = true;
              return;
            }
            int sec_addr = dono->sec_base + dono_pagina * TAM_PAGINA + off;
            if (mem_escreve(self->mem_secundaria, sec_addr, val) != ERR_OK) {
              console_printf("SO: erro ao escrever memoria secundaria addr %d", sec_addr);
              self->erro_interno = true;
              return;
            }
          }
        }
        // invalida a pagina na tabela do dono
        tabpag_invalida_pagina(dono->tabpag, dono_pagina);
      }
    }

    // carrega pagina faltante da memoria secundaria para o quadro
    int base_sec = proc->sec_base + pagina * TAM_PAGINA;
    for (int off = 0; off < TAM_PAGINA; off++) {
      int val;
      if (mem_le(self->mem_secundaria, base_sec + off, &val) != ERR_OK) {
        console_printf("SO: erro ao ler memoria secundaria addr %d", base_sec + off);
        self->erro_interno = true;
        return;
      }
      if (mem_escreve(self->mem, quadro * TAM_PAGINA + off, val) != ERR_OK) {
        console_printf("SO: erro ao escrever memoria fisica addr %d", quadro * TAM_PAGINA + off);
        self->erro_interno = true;
        return;
      }
    }

    // atualiza tabela de paginas do processo
    tabpag_define_quadro(proc->tabpag, pagina, quadro);
    // marca quadro como pertencente ao processo
    quadros_assign(self->quadros, quadro, proc->pid, pagina);
    proc->page_faults += 1;
    // calcula número de transferências de página (writeback da vítima + leitura)
    int transfers = 1; // sempre precisa ler a página solicitada
    if (need_writeback) transfers += 1;
    // simula o tempo de transferência: se houve escrita de volta (antes), conta mais uma transferência
    // (we detect writeback by checking if the page we selected earlier was altered using tabpag_bit_alteracao on the owner before invalidation)
    // For simplicity, inspect proc->page_faults change and approximate; a more precise implementation would record 'need_writeback'.
    // Atualiza tempo do disco e bloqueia processo
    int now = 0;
    if (es_le(self->es, D_RELOGIO_INSTRUCOES, &now) != ERR_OK) {
      console_printf("SO: erro ao ler relogio para calcular tempo de disco");
      self->erro_interno = true;
      return;
    }
    // calcular número de transferências exatas: se uma vitima foi removida e ela foi alterada, transfers++
    // (a detecção precisa usar informação salva antes da invalidação; for now, check page_faults increment heuristically)
    // A implementação acima já copiou de volta se necessário, portanto vamos apenas somar 1 extra quando removemos uma vítima que tinha bit de alteração.
    // To detect that, we can check the tabpag_bit_alteracao of the 'dono' we handled earlier — but that tabpag was invalidated. Simpler: if vitima != -1, assume worst-case (one writeback).
    if (vitima != -1) transfers +=  (/*assume possible writeback*/ 1);
    if (self->disco_livre_em <= now) self->disco_livre_em = now + transfers * self->tempo_transferencia_pagina;
    else self->disco_livre_em += transfers * self->tempo_transferencia_pagina;
    proc->blocked_until = self->disco_livre_em;
    // desescalona processo corrente (ficará NULL até ser desbloqueado)
    self->proc_corrente = NULL;
    console_printf("SO: page fault tratado PID %d pagina %d -> quadro %d (total %d), transfers=%d, bloqueado_ate=%d", proc->pid, pagina, quadro, proc->page_faults, transfers, proc->blocked_until);
  } else if (err == ERR_END_INV) {
    // Endereço físico inválido: erro grave de programação
    console_printf("SO: Endereço físico inválido (bug de programação de tabela de páginas)");
    self->erro_interno = true;
  } else {
    // Outros erros: tratar conforme necessário
    console_printf("SO: IRQ não tratada -- erro na CPU: %s (%d)", err_nome(err), self->regComplemento);
    self->erro_interno = true;
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
  // t2: deveria tratar a interrupção
  //   por exemplo, decrementa o quantum do processo corrente, quando se tem
  //   um escalonador com quantum
  // Envelhecimento (LRU aproximado): envelhece páginas do processo corrente
  if (self->proc_corrente && self->proc_corrente->tabpag) {
    tabpag_envelhece(self->proc_corrente->tabpag);
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
  int id_chamada = self->regA;
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
      console_printf("SO: chamada de sistema desconhecida (%d)", id_chamada);
      // t2: deveria matar o processo
      self->erro_interno = true;
  }
}

// implementação da chamada se sistema SO_LE
// faz a leitura de um dado da entrada corrente do processo, coloca o dado no reg A
static void so_chamada_le(so_t *self)
{
  // implementação com espera ocupada
  //   t2: deveria realizar a leitura somente se a entrada estiver disponível,
  //     senão, deveria bloquear o processo.
  //   no caso de bloqueio do processo, a leitura (e desbloqueio) deverá
  //     ser feita mais tarde, em tratamentos pendentes em outra interrupção,
  //     ou diretamente em uma interrupção específica do dispositivo, se for
  //     o caso
  // implementação lendo direto do terminal A
  //   t2: deveria usar dispositivo de entrada corrente do processo
  for (;;) {  // espera ocupada!
    int estado;
    if (es_le(self->es, D_TERM_A_TECLADO_OK, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado do teclado");
      self->erro_interno = true;
      return;
    }
    if (estado != 0) break;
    // como não está saindo do SO, a unidade de controle não está executando seu laço.
    // esta gambiarra faz pelo menos a console ser atualizada
    // t2: com a implementação de bloqueio de processo, esta gambiarra não
    //   deve mais existir.
    console_tictac(self->console);
  }
  int dado;
  if (es_le(self->es, D_TERM_A_TECLADO, &dado) != ERR_OK) {
    console_printf("SO: problema no acesso ao teclado");
    self->erro_interno = true;
    return;
  }
  // escreve no reg A do processador
  // (na verdade, na posição onde o processador vai pegar o A quando retornar da int)
  // t2: se houvesse processo, deveria escrever no reg A do processo
  // t2: o acesso só deve ser feito nesse momento se for possível; se não, o processo
  //   é bloqueado, e o acesso só deve ser feito mais tarde (e o processo desbloqueado)
  self->regA = dado;
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo
static void so_chamada_escr(so_t *self)
{
  // implementação com espera ocupada
  //   t2: deveria bloquear o processo se dispositivo ocupado
  // implementação escrevendo direto do terminal A
  //   t2: deveria usar o dispositivo de saída corrente do processo
  for (;;) {
    int estado;
    if (es_le(self->es, D_TERM_A_TELA_OK, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado da tela");
      self->erro_interno = true;
      return;
    }
    if (estado != 0) break;
    // como não está saindo do SO, a unidade de controle não está executando seu laço.
    // esta gambiarra faz pelo menos a console ser atualizada
    // t2: não deve mais existir quando houver suporte a processos, porque o SO não poderá
    //   executar por muito tempo, permitindo a execução do laço da unidade de controle
    console_tictac(self->console);
  }
  int dado;
  // está lendo o valor de X e escrevendo o de A direto onde o processador colocou/vai pegar
  // t2: deveria usar os registradores do processo que está realizando a E/S
  // t2: caso o processo tenha sido bloqueado, esse acesso deve ser realizado em outra execução
  //   do SO, quando ele verificar que esse acesso já pode ser feito.
  dado = self->regX;
  if (es_escreve(self->es, D_TERM_A_TELA, dado) != ERR_OK) {
    console_printf("SO: problema no acesso à tela");
    self->erro_interno = true;
    return;
  }
  self->regA = 0;
}

// implementação da chamada se sistema SO_CRIA_PROC
// cria um processo
static void so_chamada_cria_proc(so_t *self)
{
  // ainda sem suporte a processos, carrega programa e passa a executar ele
  // quem chamou o sistema não vai mais ser executado, coitado!
  // t2: deveria criar um novo processo
  // t3: identifica direito esses processos
  processo_t *processo_criador = NULL;
  processo_t *processo_criado = NULL;

  // em X está o endereço onde está o nome do arquivo
  int ender_proc;
  // t2: deveria ler o X do descritor do processo criador
  ender_proc = self->regX;
  char nome[100];
  if (so_copia_str_do_processo(self, 100, nome, ender_proc, processo_criador)) {
    int ender_carga = so_carrega_programa(self, processo_criado, nome);
    if (ender_carga != -1) {
      // t2: deveria escrever no PC do descritor do processo criado
      self->regPC = ender_carga;
      return;
    } // else?
  }
  // deveria escrever -1 (se erro) ou o PID do processo criado (se OK) no reg A
  //   do processo que pediu a criação
  self->regA = -1;
}

// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
  // t2: deveria matar um processo
  // ainda sem suporte a processos, retorna erro -1
  console_printf("SO: SO_MATA_PROC não implementada");
  self->regA = -1;
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
static void so_chamada_espera_proc(so_t *self)
{
  // t2: deveria bloquear o processo se for o caso (e desbloquear na morte do esperado)
  // ainda sem suporte a processos, retorna erro -1
  console_printf("SO: SO_ESPERA_PROC não implementada");
  self->regA = -1;
}


// ---------------------------------------------------------------------
// CARGA DE PROGRAMA {{{1
// ---------------------------------------------------------------------

// funções auxiliares
static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa);
static int so_carrega_programa_na_memoria_virtual(so_t *self, programa_t *programa, processo_t *processo);

// carrega o programa na memória
// se processo for NENHUM_PROCESSO, carrega o programa na memória física
//   senão, carrega na memória virtual do processo
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, processo_t *processo, char *nome_do_executavel)
{
  console_printf("SO: carga de '%s'", nome_do_executavel);

  programa_t *programa = prog_cria(nome_do_executavel);
  if (programa == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_carga;
  if (processo == NULL) {
    end_carga = so_carrega_programa_na_memoria_fisica(self, programa);
  } else {
    end_carga = so_carrega_programa_na_memoria_virtual(self, programa, processo);
  }

  prog_destroi(programa);
  return end_carga;
}

static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa)
{
  int end_ini = prog_end_carga(programa);
  int end_fim = end_ini + prog_tamanho(programa);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(programa, end)) != ERR_OK) {
      console_printf("Erro na carga da memória, endereco %d\n", end);
      return -1;
    }
  }

  console_printf("SO: carga na memória física %d-%d", end_ini, end_fim);
  return end_ini;
}


static int so_carrega_programa_na_memoria_virtual(so_t *self,
                                                  programa_t *programa,
                                                  processo_t *proc)
{
  // Carrega o programa na memória secundária e inicializa a tabela de páginas do processo
  int end_virt_ini = prog_end_carga(programa);
  if ((end_virt_ini % TAM_PAGINA) != 0) return -1;
  int end_virt_fim = end_virt_ini + prog_tamanho(programa) - 1;
  int pagina_ini = end_virt_ini / TAM_PAGINA;
  int pagina_fim = end_virt_fim / TAM_PAGINA;
  int n_paginas = pagina_fim - pagina_ini + 1;

  // Aloca espaço contíguo na memória secundária para o programa
  // (Aqui, para simplificação, assume-se que a memória secundária tem espaço suficiente e começa em 0)
  int end_sec_ini = proc->pid * 10000; // Exemplo: cada processo tem "faixa" de 10000 posições
  for (int i = 0; i < prog_tamanho(programa); i++) {
    if (mem_escreve(self->mem_secundaria, end_sec_ini + i, prog_dado(programa, end_virt_ini + i)) != ERR_OK) {
      console_printf("Erro na carga da memória secundária, end %d\n", end_sec_ini + i);
      return -1;
    }
  }

  // registra a base na memória secundária e zera contador de faltas
  proc->sec_base = end_sec_ini;
  proc->page_faults = 0;

  // Inicializa a tabela de páginas do processo: todas as páginas inválidas
  for (int p = pagina_ini; p <= pagina_fim; p++) {
    tabpag_invalida_pagina(proc->tabpag, p);
  }

  console_printf("SO: carga na memória secundária V%d-%d S%d-%d npag=%d",
                 end_virt_ini, end_virt_fim, end_sec_ini, end_sec_ini + prog_tamanho(programa) - 1, n_paginas);
  return end_virt_ini;
}


// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

// copia uma string da memória do processo para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// O endereço é um endereço virtual de um processo.
// t3: Com memória virtual, cada valor do espaço de endereçamento do processo
//   pode estar em memória principal ou secundária (e tem que achar onde)
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam], int end_virt, processo_t *processo)
{
  if (processo == NULL) return false;
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    // não tem memória virtual implementada, posso usar a mmu para traduzir
    //   os endereços e acessar a memória, porque todo o conteúdo do processo
    //   está na memória principal, e só temos uma tabela de páginas
    if (mmu_le(self->mmu, end_virt + indice_str, &caractere, usuario) != ERR_OK) {
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

// vim: foldmethod=marker
