// sec_alloc.h
// alocador simples para memória secundária (contíguo, bitmap)

#ifndef SEC_ALLOC_H
#define SEC_ALLOC_H

#include "memoria.h"

typedef struct sec_alloc_t sec_alloc_t;

// cria alocador para a memória secundária 'mem'
sec_alloc_t *sec_alloc_cria(mem_t *mem);
// destroi alocador
void sec_alloc_destroi(sec_alloc_t *self);
// aloca 'n' palavras contíguas na memória secundaria, retorna base ou -1
int sec_alloc_alloc(sec_alloc_t *self, int n);
// libera a região [base, base+n-1]
void sec_alloc_free(sec_alloc_t *self, int base, int n);
// retorna o tamanho total da memória secundaria (em palavras)
int sec_alloc_size(sec_alloc_t *self);

#endif // SEC_ALLOC_H
