// sec_alloc.c
// implementação simples de alocador por bitmap para memória secundária

#include "sec_alloc.h"
#include "memoria.h"
#include <stdlib.h>
#include <string.h>

struct sec_alloc_t {
  mem_t *mem;
  int size;
  unsigned char *bitmap; // 0 = free, 1 = used
};

sec_alloc_t *sec_alloc_cria(mem_t *mem)
{
  if (!mem) return NULL;
  sec_alloc_t *s = malloc(sizeof(*s));
  if (!s) return NULL;
  s->mem = mem;
  s->size = mem_tam(mem);
  s->bitmap = malloc(s->size);
  if (!s->bitmap) { free(s); return NULL; }
  memset(s->bitmap, 0, s->size);
  return s;
}

void sec_alloc_destroi(sec_alloc_t *self)
{
  if (!self) return;
  if (self->bitmap) free(self->bitmap);
  free(self);
}

int sec_alloc_alloc(sec_alloc_t *self, int n)
{
  if (!self || n <= 0 || n > self->size) return -1;
  int run = 0;
  for (int i = 0; i < self->size; i++) {
    if (self->bitmap[i] == 0) {
      run++;
      if (run == n) {
        int base = i - n + 1;
        for (int j = 0; j < n; j++) self->bitmap[base + j] = 1;
        return base;
      }
    } else {
      run = 0;
    }
  }
  return -1;
}

void sec_alloc_free(sec_alloc_t *self, int base, int n)
{
  if (!self || base < 0 || n <= 0 || base + n > self->size) return;
  for (int i = 0; i < n; i++) self->bitmap[base + i] = 0;
}

int sec_alloc_size(sec_alloc_t *self)
{
  if (!self) return 0;
  return self->size;
}
