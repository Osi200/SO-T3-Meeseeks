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

/*Evaluador de operaciones*/
#include <ctype.h>  /* isdigit */

/* Errores de exec*/
#include <errno.h>  /* para distinguir errores de exec */

/*El arbol REAL*/

#define AGING     5.0   /* cuanto sube D por nivel (la tarea se simplifica) */
#define NERF_DADO 0.1   /* baja la probabilidad del dado */
#define MAX_NIVEL 5 /* quitar despues */

static void meeseeks_arbol(const char *solicitud, double D, int N, int i);




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

/*------------------------Arbol real--------------------------------*/


static double rand01(void) { return rand() / (RAND_MAX + 1.0); }

/* Crea los hijos de un Meeseeks, TODOS juntos (en paralelo), y espera a que terminen */
static void crear_hijos(const char *solicitud, double D, int N) {



   int hijos;  /* regla del enunciado */
        if (D <= 45.0) {
            hijos = 5;      /* tarea muy dificil: pide 5 ayudantes */
        } 
        else {
            hijos = 2;      /* tarea media (45 a 85): pide 2 ayudantes */
        }
        
        
   double D_hijo; 
    if (D == 0) {
            D_hijo = 0;   /* aging: 0 sigue imposible */  
        }
        else {
            D_hijo = D + AGING;     
        } 
    if (D_hijo > 100.0) D_hijo = 100.0;


    /*---------------------------PIPE-------------------------------------------*/
   int fd[5][2];   /* un pipe por hijo: fd[k][0] lectura, fd[k][1] escritura */

    for (int k = 0; k < hijos; k++) {
        if (pipe(fd[k]) < 0) { perror("pipe"); break; }   /* el pipe se crea ANTES del fork */

        pid_t p = fork();
        if (p < 0) { perror("fork"); break; }

        if (p == 0) {
            /* HIJO */
            close(fd[k][1]);                       /* no escribe en este pipe */
            for (int j = 0; j < k; j++) {          /* cierra los pipes de hermanos anteriores que heredo */
                close(fd[j][1]);
            }
            char buf[256];
            ssize_t n = read(fd[k][0], buf, sizeof(buf) - 1);   /* recibe la solicitud */
            close(fd[k][0]);
            if (n < 0) n = 0;
            buf[n] = '\0';
            srand(getpid());
            meeseeks_arbol(buf, D_hijo, N + 1, k + 1);   /* usa lo que LEYO, no la copia heredada */
        }

        /* PADRE */
        close(fd[k][0]);                           /* no lee de este pipe */
        write(fd[k][1], solicitud, strlen(solicitud));   /* le manda la solicitud */
        close(fd[k][1]);                           /* cerrar avisa "fin del mensaje" al hijo */
    }
    while (wait(NULL) > 0);
}

/* Un Meeseeks del caso A: saluda, tira el dado y, si falla, pide ayuda. */
static void meeseeks_arbol(const char *solicitud, double D, int N, int i) {
    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d) D=%.1f\n",
           getpid(), getppid(), N, i, D);
    printf("[pid %d] Mi solicitud (por pipe): %s\n", getpid(), solicitud);
    /*printf("[pid %d] Mi solicitud (por pipe): %s\n", getpid(), solicitud);*/
    fflush(stdout);                           /* antes de cualquier fork */

    if (D > 85.0 || rand01() * 100.0 < D * NERF_DADO || N >= MAX_NIVEL) {
        printf("[pid %d] Resolvi la tarea\n", getpid());
        exit(EXIT_SUCCESS);
    }
    crear_hijos(solicitud, D, N);
    exit(EXIT_SUCCESS);
}



 /*--------BOX BASE-----*/

static void saludar(int N, int i) {
    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d)\n",
           getpid(), getppid(), N, i);
    fflush(stdout);
}


 /*  HIIIIIII IM MR MEESEEKS!!!!!!! LOOOOK AT MEEEEEEEEEEEEEEEEEEEEEE*/
/* Lo que hace un Meeseeks: saluda, atiende y desaparece */
static void meeseeks(const char *solicitud, int N, int i) {
    
/*---------------------------------------------------------------------------------------------------------------------*/
    /* Caso B: solicitud de calculo, la resuelve un solo Meeseeks */
    if (strncmp(solicitud, "calc ", 5) == 0) {
        saludar(N, i);
        long long res;
        if (evaluar(solicitud + 5, &res) == 0) {
            printf("[pid %d, ppid %d] Resultado: %lld\n", getpid(), getppid(), res);
            exit(EXIT_SUCCESS);   /* el codigo de salida le dice a Box que funciono */
        }
        printf("[pid %d, ppid %d] Error: %s\n", getpid(), getppid(), msg_error);
        exit(EXIT_FAILURE);       /* y este que fallo */
    }

    /*-------------------------------------------------------------------------------------------------------------------*/
        /* Caso C: programa externo, ejemplo "exec ls /tmp" */
    if (strncmp(solicitud, "exec ", 5) == 0) {
        saludar(N, i);
        /* Copiamos el texto porque strtok lo modifica */
        char copia[256];
        strncpy(copia, solicitud + 5, sizeof(copia) - 1);
        copia[sizeof(copia) - 1] = '\0';

        /* Partimos "ls /tmp" en un arreglo: {"ls", "/tmp", NULL} 
        strtok, parte el texto por espacio, es el formato que ocupa execvp
 
        */
        char *args[32];
        int n = 0;
        char *tok = strtok(copia, " ");
        while (tok != NULL && n < 31) {
            args[n++] = tok;
            tok = strtok(NULL, " ");
        }
        args[n] = NULL;  /* execvp exige que el arreglo termine en NULL */

        if (n == 0) {
            printf("[pid %d, ppid %d] Error: no indicaste ningun programa\n",
                   getpid(), getppid());
            exit(EXIT_FAILURE);
        }

        /* El Meeseeks necesita su propio fork: exec reemplaza el proceso entero,
           y si lo hiciera directo, desapareceria sin poder reportar nada 
           
           Tres niveles: Box, el Meeseeks y el programa externo. 
           El Meeseeks se queda esperando con waitpid, igual que Box hace con él.*/
        pid_t pid_ext = fork();
        if (pid_ext < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if (pid_ext == 0) {
            execvp(args[0], args);   /* si funciona, esta linea nunca retorna */
            /* Si llegamos aqui, exec fallo (programa no existe, sin permisos...) */
            fprintf(stderr, "[pid %d] No se pudo ejecutar '%s'\n", getpid(), args[0]);
            _exit(127);              /* 127 = convencion de shell para "comando no encontrado" 
            si el programa no existe, ese hijo es una copia del Meeseeks y no debe seguir corriendo su código. Es el mismo problema del exit  de antes.*/
        }

        int est;
        waitpid(pid_ext, &est, 0);
        if (WIFEXITED(est) && WEXITSTATUS(est) == 0) {
            printf("[pid %d, ppid %d] Programa '%s' termino con exito (codigo 0)\n",
                   getpid(), getppid(), args[0]);
            exit(EXIT_SUCCESS);
        }
        if (WIFEXITED(est))
            printf("[pid %d, ppid %d] Programa '%s' fallo (codigo %d)\n",
                   getpid(), getppid(), args[0], WEXITSTATUS(est));
        else
            printf("[pid %d, ppid %d] Programa '%s' murio por una senal\n",
                   getpid(), getppid(), args[0]);
        exit(EXIT_FAILURE);
    }

    /* Caso A: solicitud de texto, dificultad aleatoria por ahora */
    srand(getpid() ^ time(NULL));
    double D = rand() % 101;
    
    meeseeks_arbol(solicitud, D, N, i);
    exit(EXIT_SUCCESS);
}
/*-------------------------------------------------------------------------------------------------------------------*/

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
        N=1, i=1: primero del primer nivel.*/

        int estado;
        waitpid(pid, &estado, 0);
        /* WEXITSTATUS saca el codigo que el hijo paso a exit() */
        int ok = WIFEXITED(estado) && WEXITSTATUS(estado) == 0;
        printf("%s | Duración: %.4f s\n", ok ? "Completada" : "No completada", ahora() - t0);
    }

    printf("Box terminó.\n");
    return 0;
}