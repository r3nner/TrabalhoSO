// quadros.c
// implementação simples do gerenciador de quadros (FIFO)

#include "quadros.h"
#include "mmu.h" // para TAM_PAGINA
#include <stdlib.h>
#include <assert.h>

struct quadros_t {
  int n_quadros;
  int *owner_pid;
  int *owner_pagina;
  int *fifo_queue; // ring buffer
  int fifo_head;
  int fifo_tail;
  int fifo_count;
  char *is_free;
};

quadros_t *quadros_cria(mem_t *mem)
{
  quadros_t *q = malloc(sizeof(*q));
  assert(q != NULL);
  int tam = mem_tam(mem);
  int n = tam / TAM_PAGINA;
  q->n_quadros = n;
  q->owner_pid = malloc(n * sizeof(int));
  q->owner_pagina = malloc(n * sizeof(int));
  q->fifo_queue = malloc(n * sizeof(int));
  q->is_free = malloc(n * sizeof(char));
  assert(q->owner_pid && q->owner_pagina && q->fifo_queue && q->is_free);
  for (int i = 0; i < n; i++) {
    q->owner_pid[i] = -1;
    q->owner_pagina[i] = -1;
    q->is_free[i] = 1;
  }
  q->fifo_head = 0;
  q->fifo_tail = 0;
  q->fifo_count = 0;
  return q;
}

void quadros_destroi(quadros_t *self)
{
  if (!self) return;
  if (self->owner_pid) free(self->owner_pid);
  if (self->owner_pagina) free(self->owner_pagina);
  if (self->fifo_queue) free(self->fifo_queue);
  if (self->is_free) free(self->is_free);
  free(self);
}

int quadros_encontra_livre(quadros_t *self)
{
  for (int i = 0; i < self->n_quadros; i++) {
    if (self->is_free[i]) return i;
  }
  return -1;
}

int quadros_seleciona_vitima(quadros_t *self)
{
  if (self->fifo_count == 0) return -1;
  return self->fifo_queue[self->fifo_head];
}

int quadros_seleciona_vitima_lru(quadros_t *self, bool (*get_age_cb)(int pid, int pagina, unsigned *page_age, void *user_data), void *user_data)
{
  if (self->fifo_count == 0) return -1;
  int best = -1;
  unsigned best_age = 0xffffffffu; // choose smallest age
  for (int i = 0; i < self->n_quadros; i++) {
    if (self->is_free[i]) continue;
    int pid = self->owner_pid[i];
    int pagina = self->owner_pagina[i];
    unsigned age;
    if (!get_age_cb(pid, pagina, &age, user_data)) continue;
    if (best == -1 || age < best_age) {
      best = i;
      best_age = age;
    }
  }
  // if no candidate found (callback failed), fall back to FIFO
  if (best == -1) return quadros_seleciona_vitima(self);
  return best;
}

int quadros_remove_vitima(quadros_t *self)
{
  if (self->fifo_count == 0) return -1;
  int v = self->fifo_queue[self->fifo_head];
  self->fifo_head = (self->fifo_head + 1) % self->n_quadros;
  self->fifo_count--;
  // marca livre
  self->is_free[v] = 1;
  self->owner_pid[v] = -1;
  self->owner_pagina[v] = -1;
  return v;
}

void quadros_free_pid(quadros_t *self, int pid)
{
  if (!self) return;
  // mark frames owned by pid as free
  for (int i = 0; i < self->n_quadros; i++) {
    if (!self->is_free[i] && self->owner_pid[i] == pid) {
      self->is_free[i] = 1;
      self->owner_pid[i] = -1;
      self->owner_pagina[i] = -1;
    }
  }
  // rebuild FIFO queue to remove freed frames
  int new_tail = 0;
  int new_count = 0;
  for (int i = 0; i < self->fifo_count; i++) {
    int idx = (self->fifo_head + i) % self->n_quadros;
    int q = self->fifo_queue[idx];
    if (!self->is_free[q]) {
      self->fifo_queue[new_tail++] = q;
      new_count++;
    }
  }
  self->fifo_head = 0;
  self->fifo_tail = new_tail % self->n_quadros;
  self->fifo_count = new_count;
}

int quadros_numero_quadros(quadros_t *self)
{
  if (!self) return 0;
  return self->n_quadros;
}

void quadros_assign(quadros_t *self, int quadro, int pid, int pagina)
{
  if (quadro < 0 || quadro >= self->n_quadros) return;
  self->owner_pid[quadro] = pid;
  self->owner_pagina[quadro] = pagina;
  self->is_free[quadro] = 0;
  // insere na fila (tail)
  if (self->fifo_count < self->n_quadros) {
    self->fifo_queue[self->fifo_tail] = quadro;
    self->fifo_tail = (self->fifo_tail + 1) % self->n_quadros;
    self->fifo_count++;
  } else {
    // fila cheia: sobrescreve posição head (defensivo)
    self->fifo_queue[self->fifo_tail] = quadro;
    self->fifo_tail = (self->fifo_tail + 1) % self->n_quadros;
    self->fifo_head = self->fifo_tail; // move head para manter invariantes
  }
}

int quadros_owner_pid(quadros_t *self, int quadro)
{
  if (quadro < 0 || quadro >= self->n_quadros) return -1;
  return self->owner_pid[quadro];
}

int quadros_owner_pagina(quadros_t *self, int quadro)
{
  if (quadro < 0 || quadro >= self->n_quadros) return -1;
  return self->owner_pagina[quadro];
}
