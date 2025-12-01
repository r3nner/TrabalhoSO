// config.h
// parametros globais de configuracao do simulador
// simulador de computador
// so25b

#ifndef CONFIG_H
#define CONFIG_H

typedef enum {
  SUBSTITUICAO_LRU,
  SUBSTITUICAO_FIFO
} substituicao_algoritmo_t;

// tamanho da memoria principal (em palavras)
#define CONFIG_TAM_MEMORIA_PRINCIPAL 200

// tamanho da pagina da MMU (em palavras)
#define CONFIG_TAM_PAGINA 10

// algoritmo padrao de substituicao de paginas
#define CONFIG_ALGORITMO_SUBSTITUICAO SUBSTITUICAO_LRU

// tempo fixo (em instrucoes) para transferir uma pagina entre memoria
// principal e secundaria
#define CONFIG_TEMPO_TRANSFERENCIA_PAGINA 30

// fator multiplicador do tamanho da memoria secundaria em relacao a principal
#define CONFIG_FATOR_MEM_SECUNDARIA 4

#endif // CONFIG_H
