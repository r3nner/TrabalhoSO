# FALTA_FAZER

## Visão Geral do T3 Implementado

- MMU e tabelas de página por processo ativas: `so_proc_inicializa_vm` cria a tabela, `so_mmu_define_tabpag` troca no despacho.
- Carga inicial vai para a memória secundária (`so_carrega_programa`), mantendo slots em `indices_pagsec`.
- page fault tratado em `so_atende_falta_pagina`, com swap sob demanda e bloqueio temporizado via `so_vm_agenda_transferencia`.
- Algoritmos FIFO/LRU selecionáveis em `config.h` (`CONFIG_ALGORITMO_SUBSTITUICAO`). LRU usa envelhecimento em `so_vm_atualiza_idade_quadros`.
- Quadros reservados (PID < 0) protegidos durante a substituição; `so_vm_salva_quadro` aborta se tentar reciclar esses quadros.
- Métricas globais e por processo exibidas no relatório final (`so_imprime_relatorio_final`).

## Checklist para Testes Robustos

1. **Build limpo**
   - `make clean` (se disponível) e `make`.
   - Verificar ausência de warnings/erros.
2. **Baseline (config padrão)**
   - `./main init.maq`
   - Ao finalizar, rodar `tail -n 40 log_da_console`.
   - Conferir falhas de página, tempos e ausência de mensagens de erro.
3. **Memória reduzida à metade**
   - Editar `config.h`: `CONFIG_TAM_MEMORIA_PRINCIPAL` → metade do valor original.
   - `make` e `./main init.maq`.
   - Registrar: número de falhas, tempos, desbloqueios por página.
4. **Memória mínima viável**
   - Repetir redução até o limite (manter ao menos quadros reservados + 1).
   - Testar comportamento quando não há quadro elegível (espera mensagens e bloqueio seguro).
5. **Variação do tamanho de página**
   - `CONFIG_TAM_PAGINA` pequeno (ex.: 4) e grande (≥ 4× o padrão).
   - Recompilar e rodar testes, comparando falhas e tempo total.
6. **Algoritmo FIFO vs LRU / Escalonadores**
   - Alternar `CONFIG_ALGORITMO_SUBSTITUICAO` entre `SUBSTITUICAO_FIFO` e `SUBSTITUICAO_LRU`.
   - Para cada algoritmo, rodar tanto o Round Robin quanto o escalonador por prioridade.
   - Registrar diferenças de substituição (consultar logs para carimbo FIFO vs idade LRU) e comportamento do escalonamento.
7. **Stress de criação de processos**
   - Rodar programas que criem filhos (`p1.maq`, `p2.maq` etc.) após `init`.
   - Verificar fila de prontos e desbloqueios em `log_da_console`.

## Geração e Registro de Relatórios

1. Após cada execução: `tail -n 60 log_da_console > resultados/<cenario>.log`.
2. Extrair estatísticas principais: tempo total, tempo ocioso, falhas de página, transferências e métricas por PID.
3. Para comparação, montar planilha simples (PID × falhas × tempo retorno × algoritmo × tamanho de memória/página).
4. Atualizar `README.md` com descrição dos cenários testados, parâmetros usados e observações sobre desempenho.
5. Manter histórico do `config.h` (por exemplo, salvar cópias `config_memX_pagY.h`), facilitando reproduzir cada bateria de testes.

## Itens Restantes

- Documentar no Relatório do T3 os experimentos exigidos (memória/ página/ algoritmo) usando os logs coletados.
- Validar comportamento com interrupções de E/S intensivas (terminais) e garantir desbloqueio correto.
- Revisar dependências/headers se novos arquivos forem adicionados para relatório automatizado.
- Conferir se o relatório do T2 está completo, incluindo execuções com Round Robin e Prioridade, além da análise dos tempos.
- No relatório do T3, comparar explicitamente os modos de substituição da MMU (LRU e FIFO) em múltiplas configurações.
- Excluir o T3 do repositório e renomear T3-DO-0 para T3

## Próximos Passos de Coordenação

- Confirmar com o pessoal que já testou o T3 quais cenários eles cobriram e reproduzir os passos.
- Quando todos os testes estiverem concluídos, enviar email novamente para o BenHur
