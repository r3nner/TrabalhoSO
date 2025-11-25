; teste_espera.asm
; Programa de teste para SO_ESPERA_PROC
; Cria um filho, espera ele terminar, imprime mensagem

         desv main
prog     string 'teste_espera: criando filho...'
msg2     string 'filho criado com PID='
msg3     string 'esperando filho...'
msg4     string 'filho terminou! OK'
filho    string 'p1.maq'

; chamadas de sistema
SO_ESCR        define 2
SO_CRIA_PROC   define 7
SO_MATA_PROC   define 8
SO_ESPERA_PROC define 9

main
         ; Mensagem inicial
         cargi prog
         chama impstr
         cargi ' '
         chama impch

         ; Cria processo filho
         cargi filho
         trax
         cargi SO_CRIA_PROC
         chamas
         
         ; Salva PID do filho
         armm pid_filho
         
         ; Imprime PID
         cargi msg2
         chama impstr
         cargm pid_filho
         chama impnum
         cargi ' '
         chama impch

         ; Mensagem
         cargi msg3
         chama impstr
         cargi ' '
         chama impch

         ; Espera o filho terminar (pode bloquear!)
         cargm pid_filho
         trax
         cargi SO_ESPERA_PROC
         chamas
         
         ; Filho terminou
         cargi msg4
         chama impstr
         cargi ' '
         chama impch

         ; Termina
         chama morre

morre    espaco 1
         cargi 0
         trax
         cargi SO_MATA_PROC
         chamas
         ret morre

pid_filho espaco 1

; imprime a string que inicia em A
impstr   espaco 1
         trax
impstr1
         cargx 0
         desvz impstrf
         chama impch
         incx
         desv impstr1
impstrf  ret impstr

; imprime caractere em A
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

; escreve o valor de A no terminal, em decimal
impnum  espaco 1
        armm ei_num
        desvp ei_pos
        desvn ei_neg
        cargi '0'
        chama impch
        desv ei_f
ei_neg
        neg
        armm ei_num
        cargi '-'
        chama impch
ei_pos
        cargi 1
        armm ei_mul
ei_1
        cargm ei_mul
        sub ei_num
        desvz ei_3
        desvp ei_2
        cargm ei_mul
        mult dez
        armm ei_mul
        desv ei_1
ei_2
        cargm ei_mul
        div dez
        armm ei_mul
ei_3
        cargm ei_num
        div ei_mul
        resto dez
        soma a_zero
        chama impch
        cargm ei_mul
        div dez
        armm ei_mul
        desvp ei_3
ei_f
        ret impnum
ei_num  espaco 1
ei_mul  espaco 1
a_zero  valor '0'
dez     valor 10
