/*
 * Instituto Tecnológico de Costa Rica
 * Escuela de Ingeniería en Computación
 * IC-6600 Principios de Sistemas Operativos
 * Tarea 3 - Mr. Meeseeks
 * Estudiante: Oscar Andrés Bezara Pérez, Carné 2024075088
 * Fecha:  9 de octubre 2026
 */


 /*--------BOX BASE-----*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <ctype.h>  /* isdigit */


 /*--------BOX BASE-----*/
/* Como la tarea evalua tiempos, es importante que estos valores se mantengan estables, asi que que tenga un reloj independiente al de la computadora. */
static double ahora(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}



/* ---------- Evaluador de operaciones (caso B) ---------- */
/* Precedencia, de menor a mayor: |  ^  &  + -  * / %  unarios (- ~ !)  ( ) */

static const char *cur;        /* 
curr es un puntero que recorre el texto viendo cual es el *current* caracter


*/
static int hubo_error;         /* algo salio mal */
static const char *msg_error;  /* que salio mal */

static void error(const char *m) { hubo_error = 1; msg_error = m; }
static void saltar(void) { while (*cur == ' ') cur++; }

static long long expr_or(void);  /* declaracion adelantada: los parentesis la necesitan */



/*PRIMARIO:

va de basico a complejo, este es el elemental

este primario se encarga de parentesis y busca todo lo que esta dentro del parentesis actual

digamos, cuando ve (, va a buscar ). Si ve otro (, como si fuera (1+(2+3)), abre expr_or y ahora
se encarga desde ese (2+3)

*/

static long long primario(void) {
    saltar();
    if (*cur == '(') {            /* un parentesis reinicia la precedencia */
        cur++;
        long long v = expr_or();
        saltar();
        if (*cur != ')') { error("falta ')'"); return 0; }
        cur++;
        return v;
    }
    if (isdigit((unsigned char)*cur)) {
        char *fin;
        long long v = strtoll(cur, &fin, 10);  /* lee el numero y avanza cur */
        cur = fin;
        return v;
    }
    error("se esperaba un numero");
    return 0;
}


/*

signos que van pegados a un valor, como decir --1 que va a ser igual a 1 positivo.

digamos: 
1. *curr == '-'    
2.curr++ avanzo para el siguiente     
3. retorno -unario pido el valor de lo que que sigue, le cambio el digno y devuelvo

1.Se ve - de -5, llama a -unario() con curr en 5
2. como ningun no coincide el if, devuelve 5, osea unario() = 5
3. como lo que llamamos era -unario(), retrona -5

1.Se ve - de --5, llama a -unario() con curr en el segundo -.  -unairo(-5)
2. coincide y llama a -unario con curr en 5       -unario(-unario(5))
2. como ningun no coincide el if, devuelve 5, osea unario() = 5
3. queda el 5 al final

*/

static long long unario(void) {
    saltar();
    if (*cur == '-') { cur++; return -unario(); }
    if (*cur == '~') { cur++; return ~unario(); }  /* NOT bit a bit */
    if (*cur == '!') { cur++; return !unario(); }  /* NOT logico */
    return primario();
}


/*
Aqui se encargan de multiplicaciones, divisiones y %. Se hace la revision de que no se pueda hacer /0.


*/
static long long termino(void) {   /* * / % */
    long long v = unario();
    while (!hubo_error) {
        saltar();
        char op = *cur;
        if (op != '*' && op != '/' && op != '%') break;
        cur++;
        long long d = unario();
        if (op == '*') v *= d;
        else if (d == 0) { error("division entre cero"); return 0; }  /* caso clave */
        else v = (op == '/') ? v / d : v % d;
    }
    return v;
}

static long long suma(void) {      /* + - */
    long long v = termino();
    while (!hubo_error) {
        saltar();
        char op = *cur;
        if (op != '+' && op != '-') break;
        cur++;
        long long d = termino();
        v = (op == '+') ? v + d : v - d;
    }
    return v;
}

/*Todas se llaman en orden de cascada., ya que van en orden de prioridad.*/


static long long expr_and(void) {  /* & */
    long long v = suma();
    while (!hubo_error) { saltar(); if (*cur != '&') break; cur++; v &= suma(); }
    return v;
}

static long long expr_xor(void) {  /* ^ */
    long long v = expr_and();
    while (!hubo_error) { saltar(); if (*cur != '^') break; cur++; v ^= expr_and(); }
    return v;
}

static long long expr_or(void) {   /* | */
    long long v = expr_xor();
    while (!hubo_error) { saltar(); if (*cur != '|') break; cur++; v |= expr_xor(); }
    return v;
}

/* Devuelve 0 si salio bien (resultado en *res), 1 si hubo error */
static int evaluar(const char *texto, long long *res) {
    cur = texto;
    hubo_error = 0;
    *res = expr_or();
    saltar();
    if (!hubo_error && *cur != '\0') error("caracteres sobrantes");
    return hubo_error;
}
 /*--------BOX BASE-----*/
/* Lo que hace un Meeseeks: saluda, atiende y desaparece */
static void meeseeks(const char *solicitud, int N, int i) {
    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d)\n",
           getpid(), getppid(), N, i);

    /* Caso B: solicitud de calculo, la resuelve un solo Meeseeks */
    if (strncmp(solicitud, "calc ", 5) == 0) {
        long long res;
        if (evaluar(solicitud + 5, &res) == 0) {
            printf("[pid %d, ppid %d] Resultado: %lld\n", getpid(), getppid(), res);
            exit(EXIT_SUCCESS);   /* el codigo de salida le dice a Box que funciono */
        }
        printf("[pid %d, ppid %d] Error: %s\n", getpid(), getppid(), msg_error);
        exit(EXIT_FAILURE);       /* y este que fallo */
    }

    printf("[pid %d, ppid %d] Atendiendo: %s\n", getpid(), getppid(), solicitud);
    exit(EXIT_SUCCESS);
}

 /*--------BOX BASE-----*/
int main(void) {
    char linea[256]; /* lo que quiera guardar el usuario*/

    while (1) { /*  la box puede recibir solicitudes siempre, es decision del usuario hacer o no el break*/
        printf("Box> ");
        fflush(stdout);  /* Es para que el texto aparezca rapido, porque se le podria pasar a un hijo y salir dos veces por temas del  buffer*/

        if (fgets(linea, sizeof(linea), stdin) == NULL) break;
        linea[strcspn(linea, "\n")] = '\0';  /* para limpiar la linea */

        if (strcmp(linea, "adios") == 0) break; /*Nuestro break, despedirse*/
        if (linea[0] == '\0') continue;

        double t0 = ahora();
        pid_t pid = fork();  /* aquí nace el Meeseeks */

        if (pid < 0) {
            perror("fork");  /* Falla el fork*/
            continue;
        }
        if (pid == 0) meeseeks(linea, 1, 1);  /* Es el hijo
        N=1, i=1: primero del primer nivel*/

        int estado;
        waitpid(pid, &estado, 0);
        /* WEXITSTATUS saca el codigo que el hijo paso a exit() */
        int ok = WIFEXITED(estado) && WEXITSTATUS(estado) == 0;
        printf("%s | Duración: %.4f s\n", ok ? "Completada" : "No completada", ahora() - t0);
    }

    printf("Box terminó.\n");
    return 0;
}