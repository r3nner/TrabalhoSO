; teste_le.asm
; Programa de teste para SO_LE (leitura com bloqueio)
; Lê 5 caracteres do terminal e imprime cada um

N_LEITURAS define 5

         desv main
prog     string 'teste_le: aguardando 5 caracteres... '

; chamadas de sistema
SO_LE          define 1
SO_ESCR        define 2
SO_MATA_PROC   define 8

main
         ; Imprime mensagem inicial
         cargi prog
         chama impstr
         cargi '['
         chama impch

         ; Contador de leituras
         cargi 0
         armm contador

laco_le
         ; Lê um caractere (pode bloquear!)
         cargi SO_LE
         chamas
         ; Caractere lido está em A
         
         ; Imprime o caractere lido
         chama impch
         
         ; Incrementa contador
         cargm contador
         soma um
         armm contador
         
         ; Verifica se já leu N_LEITURAS
         sub n_leituras
         desvn laco_le

         ; Termina
         cargi ']'
         chama impch
         cargi ' '
         chama impch
         cargi 'O'
         chama impch
         cargi 'K'
         chama impch
         
         chama morre

morre    espaco 1
         cargi 0
         trax
         cargi SO_MATA_PROC
         chamas
         ret morre

contador espaco 1
um       valor 1
n_leituras valor N_LEITURAS

; imprime a string que inicia em A (destroi X)
impstr   espaco 1
         trax
impstr1
         cargx 0
         desvz impstrf
         chama impch
         incx
         desv impstr1
impstrf  ret impstr

; função que chama o SO para imprimir o caractere em A
impch    espaco 1
         trax
         armm impch_X
         cargi SO_ESCR
         chamas
         trax
         cargm impch_X
         trax
         ret impch
impch_X  espaco 1
