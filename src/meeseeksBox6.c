/*
 * Instituto Tecnológico de Costa Rica
 * Escuela de Ingeniería en Computación
 * IC-6600 Principios de Sistemas Operativos
 * Tarea 3 - Mr. Meeseeks
 * Estudiante: Oscar Andrés Bezara Pérez, Carné 2024075088
 * Fecha:  10 de octubre 2026
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <ctype.h>   /* isdigit, para el evaluador */
#include <errno.h>   /* errores de exec */

#define AGING     5.0   /* cuanto sube D por nivel (la tarea se simplifica) */
#define NERF_DADO 0.1   /* baja la probabilidad del dado */
#define MAX_NIVEL 5     /* freno temporal, quitar despues */

/* Mensaje que viaja por el pipe de vuelta: quien resolvio y donde esta en el arbol.
   Debe ir ANTES de las funciones que lo usan. */
typedef struct {
    int resuelto;          /* 1 = alguien del subarbol resolvio */
    int pid, ppid, N, i;   /* datos del Meeseeks que resolvio */
} Reporte;

static Reporte meeseeks_arbol(const char *solicitud, double D, int N, int i);

/* Reloj monotonico: no se afecta si cambian la hora del sistema */
static double ahora(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* write() con el resultado "usado", para que gcc no avise */
static void enviar(int fd, const void *datos, size_t n) {
    ssize_t w = write(fd, datos, n);
    (void)w;
}

/* ---------- Evaluador de operaciones (caso B) ---------- */
/* Precedencia, de menor a mayor: |  ^  &  + -  * / %  unarios (- ~ !)  ( ) */

static const char *cur;        /* puntero que recorre el texto: el caracter actual */
static int hubo_error;         /* algo salio mal */
static const char *msg_error;  /* que salio mal */

static void error(const char *m) { hubo_error = 1; msg_error = m; }
static void saltar(void) { while (*cur == ' ') cur++; }

static long long expr_or(void);  /* declaracion adelantada: los parentesis la necesitan */

/* Nivel basico: numeros y parentesis (un parentesis reinicia la precedencia) */
static long long primario(void) {
    saltar();
    if (*cur == '(') {
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

/* Signos pegados a un valor: - ~ !  (recursivo, por eso --5 = 5) */
static long long unario(void) {
    saltar();
    if (*cur == '-') { cur++; return -unario(); }
    if (*cur == '~') { cur++; return ~unario(); }  /* NOT bit a bit */
    if (*cur == '!') { cur++; return !unario(); }  /* NOT logico */
    return primario();
}

/* * / %  con proteccion contra division entre cero */
static long long termino(void) {
    long long v = unario();
    while (!hubo_error) {
        saltar();
        char op = *cur;
        if (op != '*' && op != '/' && op != '%') break;
        cur++;
        long long d = unario();
        if (op == '*') v *= d;
        else if (d == 0) { error("division entre cero"); return 0; }
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

/* Cascada: cada funcion pide sus operandos a la de mayor prioridad */
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

/* ---------- Arbol real (caso A) ---------- */

static double rand01(void) { return rand() / (RAND_MAX + 1.0); }

/* Crea los hijos (todos juntos, en paralelo), les manda la solicitud por el pipe
   de ida y recoge sus reportes por el pipe de vuelta.
   Devuelve el reporte final de este subarbol. */
static Reporte crear_hijos(const char *solicitud, double D, int N) {


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


    
    int ida[5][2];     /* padre -> hijo */
    int vuelta[5][2];  /* hijo -> padre */
    int creados = 0;

    for (int k = 0; k < hijos; k++) {
        /* Los pipes se crean ANTES del fork para que el hijo los herede */
        if (pipe(ida[k]) < 0 || pipe(vuelta[k]) < 0) { perror("pipe"); break; }
        pid_t p = fork();
        if (p < 0) { perror("fork"); break; }

        if (p == 0) {
            /* HIJO: cierra los extremos que no usa */
            close(ida[k][1]);      /* no escribe en el pipe de ida */
            close(vuelta[k][0]);   /* no lee del pipe de vuelta */
            for (int j = 0; j < k; j++) close(vuelta[j][0]);  /* lecturas de hermanos anteriores que heredo */

            char buf[256];
            ssize_t n = read(ida[k][0], buf, sizeof(buf) - 1);  /* recibe la solicitud */
            close(ida[k][0]);
            if (n < 0) n = 0;
            buf[n] = '\0';
            srand(getpid());       /* si no, todos los hermanos tiran el mismo dado */

            Reporte r = meeseeks_arbol(buf, D_hijo, N + 1, k + 1);
            enviar(vuelta[k][1], &r, sizeof(r));   /* reporta al padre */
            close(vuelta[k][1]);
            exit(r.resuelto ? EXIT_SUCCESS : EXIT_FAILURE);  /* el hijo nunca vuelve al codigo del padre */
        }

        /* PADRE */
        creados++;
        close(ida[k][0]);                              /* no lee de la ida */
        enviar(ida[k][1], solicitud, strlen(solicitud));
        close(ida[k][1]);                              /* cerrar avisa "fin del mensaje" */
        close(vuelta[k][1]);                           /* el padre solo lee de la vuelta */
    }

    /* Lee el reporte de cada hijo, en orden (el paso C2 lo hara con poll) */
    Reporte final = {0, 0, 0, 0, 0};
    for (int k = 0; k < creados; k++) {
        Reporte r;
        ssize_t n = read(vuelta[k][0], &r, sizeof(r));
        close(vuelta[k][0]);
        if (n == (ssize_t)sizeof(r) && r.resuelto && !final.resuelto) final = r;
    }
    while (wait(NULL) > 0);   /* recoge a todos los hijos: no quedan zombies */
    return final;
}

/* Un Meeseeks del caso A: saluda, tira el dado y, si falla, pide ayuda.
   Devuelve el reporte de su subarbol. */
static Reporte meeseeks_arbol(const char *solicitud, double D, int N, int i) {
    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d) D=%.1f\n",
           getpid(), getppid(), N, i, D);
    fflush(stdout);   /* vaciar el buffer antes de cualquier fork */

    if (D > 85.0 || rand01() * 100.0 < D * NERF_DADO || N >= MAX_NIVEL) {
        printf("[pid %d] Resolvi la tarea\n", getpid());
        fflush(stdout);
        Reporte r = {1, getpid(), getppid(), N, i};
        return r;
    }
    return crear_hijos(solicitud, D, N);
}

/* ---------- Meeseeks y Box ---------- */

static void saludar(int N, int i) {
    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d)\n",
           getpid(), getppid(), N, i);
    fflush(stdout);
}

/* Lo que hace un Meeseeks: saluda, atiende y desaparece */
static void meeseeks(const char *solicitud, int N, int i) {

    /* Caso B: solicitud de calculo, la resuelve un solo Meeseeks */
    if (strncmp(solicitud, "calc ", 5) == 0) {
        saludar(N, i);
        long long res;
        if (evaluar(solicitud + 5, &res) == 0) {
            printf("[pid %d, ppid %d] Resultado: %lld\n", getpid(), getppid(), res);
            exit(EXIT_SUCCESS);   /* el codigo de salida le dice a Box que funciono */
        }
        printf("[pid %d, ppid %d] Error: %s\n", getpid(), getppid(), msg_error);
        exit(EXIT_FAILURE);
    }

    /* Caso C: programa externo, ejemplo "exec ls /tmp" */
    if (strncmp(solicitud, "exec ", 5) == 0) {
        saludar(N, i);
        char copia[256];   /* copia porque strtok modifica el texto */
        strncpy(copia, solicitud + 5, sizeof(copia) - 1);
        copia[sizeof(copia) - 1] = '\0';

        /* Parte "ls /tmp" en {"ls", "/tmp", NULL}, el formato de execvp */
        char *args[32];
        int n = 0;
        char *tok = strtok(copia, " ");
        while (tok != NULL && n < 31) {
            args[n++] = tok;
            tok = strtok(NULL, " ");
        }
        args[n] = NULL;   /* execvp exige que el arreglo termine en NULL */

        if (n == 0) {
            printf("[pid %d, ppid %d] Error: no indicaste ningun programa\n",
                   getpid(), getppid());
            exit(EXIT_FAILURE);
        }

        /* exec reemplaza el proceso entero, asi que el Meeseeks hace su propio
           fork para poder esperar el resultado y reportarlo */
        pid_t pid_ext = fork();
        if (pid_ext < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if (pid_ext == 0) {
            execvp(args[0], args);   /* si funciona, esta linea nunca retorna */
            fprintf(stderr, "[pid %d] No se pudo ejecutar '%s'\n", getpid(), args[0]);
            _exit(127);              /* 127 = "comando no encontrado" por convencion */
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

    Reporte r = meeseeks_arbol(solicitud, D, N, i);
    if (r.resuelto) {
        printf("[pid %d, ppid %d] Tarea completada. La resolvio el Meeseeks (%d, %d, %d, %d)\n",
               getpid(), getppid(), r.pid, r.ppid, r.N, r.i);
        exit(EXIT_SUCCESS);
    }
    printf("[pid %d, ppid %d] No se pudo resolver la tarea\n", getpid(), getppid());
    exit(EXIT_FAILURE);
}

int main(void) {
    char linea[256];   /* lo que escribe el usuario */

    while (1) {   /* Box acepta solicitudes hasta que el usuario se despida */
        printf("Box> ");
        fflush(stdout);   /* evita texto duplicado en los hijos por el buffer */

        if (fgets(linea, sizeof(linea), stdin) == NULL) break;
        linea[strcspn(linea, "\n")] = '\0';   /* quita el salto de linea */

        if (strcmp(linea, "adios") == 0) break;   /* nuestra despedida */
        if (linea[0] == '\0') continue;

        double t0 = ahora();
        pid_t pid = fork();   /* aqui nace el primer Meeseeks */

        if (pid < 0) {
            perror("fork");
            continue;
        }
        if (pid == 0) meeseeks(linea, 1, 1);   /* hijo: N=1, i=1 */

        int estado;
        waitpid(pid, &estado, 0);
        int ok = WIFEXITED(estado) && WEXITSTATUS(estado) == 0;  /* codigo de salida del hijo */
        printf("%s | Duración: %.4f s\n", ok ? "Completada" : "No completada", ahora() - t0);
    }

    printf("Box terminó.\n");
    return 0;
}