// quadros.h
// gerenciador de quadros (frames) para paginação
// implementação mínima FIFO para o T3

#ifndef QUADROS_H
#define QUADROS_H

#include "memoria.h"
#include <stdbool.h>

typedef struct quadros_t quadros_t;

// cria o gerenciador de quadros para a memória física 'mem'
quadros_t *quadros_cria(mem_t *mem);

// destrói o gerenciador
void quadros_destroi(quadros_t *self);

// retorna o índice de um quadro livre, ou -1 se não houver
int quadros_encontra_livre(quadros_t *self);

// seleciona uma vítima segundo a política corrente (FIFO por enquanto)
// retorna índice do quadro vítima ou -1 se não houver
int quadros_seleciona_vitima(quadros_t *self);

// remove e retorna o próximo elemento da fila de substituição (FIFO)
// retorna -1 se fila vazia
int quadros_remove_vitima(quadros_t *self);

// atribui o quadro 'quadro' ao processo 'pid' e à página 'pagina'
// também insere o quadro na fila de substituição
void quadros_assign(quadros_t *self, int quadro, int pid, int pagina);

// obtém o pid dono do quadro (ou -1 se livre)
int quadros_owner_pid(quadros_t *self, int quadro);

// obtém a página dona do quadro (ou -1 se livre)
int quadros_owner_pagina(quadros_t *self, int quadro);

// devolve todos os quadros pertencentes ao pid (marca-los livres)
void quadros_free_pid(quadros_t *self, int pid);

// retorna o número total de quadros
int quadros_numero_quadros(quadros_t *self);

// seleciona vítima usando LRU: recebe um callback para obter a 'age' da página
// callback deve retornar true e preencher *page_age se a página existir, ou false caso contrário
// a função user_data é repassada ao callback
int quadros_seleciona_vitima_lru(quadros_t *self, bool (*get_age_cb)(int pid, int pagina, unsigned *page_age, void *user_data), void *user_data);

#endif // QUADROS_H
