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
/* PIPE CONTINUACION*/
#include <poll.h>      /* poll: vigilar varios pipes a la vez */
#include <signal.h>    /* sigaction, kill, SIGTERM */


/*Semaforos*/
#include <sys/mman.h>    /* mmap: memoria compartida */
#include <semaphore.h>   /* sem_t, sem_init, sem_wait, sem_post */




#define AGING     5.0   /* cuanto sube D por nivel (la tarea se simplifica) */
#define NERF_DADO 0.1   /* baja la probabilidad del dado */
#define MAX_NIVEL 5     /* freno temporal, quitar despues */
#define ESPERA_MAX_US 1500000 
#define MAX_NIV_STATS 64 

static pid_t mis_hijos[5];  
static volatile int num_hijos = 0;

/* Mensaje que viaja por el pipe de vuelta: quien resolvio y donde esta en el arbol.
   Debe ir ANTES de las funciones que lo usan. */
typedef struct {
    int resuelto;          /* 1 = alguien del subarbol resolvio */
    int pid, ppid, N, i;   /* datos del Meeseeks que resolvio */
} Reporte;


typedef struct {
    sem_t mutex;                    /* semaforo: solo uno a la vez toca los contadores */
    int total;                      /* Meeseeks creados para la solicitud actual */
    int max_nivel;                  /* nivel mas profundo alcanzado */
    int por_nivel[MAX_NIV_STATS];   /* cuantos Meeseeks hay en cada nivel */
} Compartido;


static Compartido *comp;


static void iniciar_compartido(void) {
    comp = mmap(NULL, sizeof(Compartido), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);   /* MAP_SHARED: el fork NO la copia, la comparte */
    if (comp == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }
    sem_init(&comp->mutex, 1, 1);   /* 2do arg 1 = compartido entre procesos; valor inicial 1 = libre */
}



/* Pone los contadores en cero antes de cada solicitud nueva */
static void reiniciar_contadores(void) {
    comp->total = 0;
    comp->max_nivel = 0;
    memset(comp->por_nivel, 0, sizeof(comp->por_nivel));
}


/* Cada Meeseeks se anota al nacer */
static void registrar(int N) {
    /* Bloqueamos SIGTERM mientras tenemos el semaforo: si el manejador hiciera
       _exit con el semaforo tomado, nadie mas podria entrar nunca (deadlock) */
    sigset_t bloq, previo;
    sigemptyset(&bloq);
    sigaddset(&bloq, SIGTERM);
    sigprocmask(SIG_BLOCK, &bloq, &previo);

    sem_wait(&comp->mutex);          /* P: entra a la seccion critica */
    comp->total++;
    if (N > comp->max_nivel) comp->max_nivel = N;
    if (N < MAX_NIV_STATS) comp->por_nivel[N]++;
    sem_post(&comp->mutex);          /* V: sale */

    sigprocmask(SIG_SETMASK, &previo, NULL);
}



/* PIPE CONTINUACION*/



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


/*  Pipe--------------------------------------------------------------------------  */

/* Se ejecuta cuando le llega SIGTERM: avisa a sus hijos, se despide y desaparece */
static void al_terminar(int sig) {
    (void)sig;
    for (int k = 0; k < num_hijos; k++) kill(mis_hijos[k], SIGTERM);
    while (wait(NULL) > 0);   /* espera a que los hijos se despidan primero */
    char msg[128];
    int n = snprintf(msg, sizeof(msg),
        "[pid %d, ppid %d] Me piden terminar. Gracias por servirte, adios!\n",
        getpid(), getppid());
    ssize_t w = write(STDOUT_FILENO, msg, n);
    (void)w;
    _exit(EXIT_FAILURE);
}

/* ---------- Arbol real (caso A) ---------- */

static double rand01(void) { return rand() / (RAND_MAX + 1.0); }

/* Crea los hijos (todos juntos, en paralelo), les manda la solicitud por el pipe
   de ida y recoge sus reportes por el pipe de vuelta.
   Devuelve el reporte final de este subarbol. */
static Reporte crear_hijos(const char *solicitud, double D, int N) {
    int hijos = (D <= 45.0) ? 5 : 2;
    double D_hijo = (D == 0) ? 0 : D + AGING;
    if (D_hijo > 100.0) D_hijo = 100.0;

    int ida[5][2], vuelta[5][2];
    int creados = 0;

    /* Bloqueamos SIGTERM alrededor del fork: asi ninguna señal llega a un hijo
       que todavia no tiene su manejador ni su lista de hijos en orden */
    sigset_t bloq, previo;
    sigemptyset(&bloq);
    sigaddset(&bloq, SIGTERM);
    num_hijos = 0;

    for (int k = 0; k < hijos; k++) {
        if (pipe(ida[k]) < 0 || pipe(vuelta[k]) < 0) { perror("pipe"); break; }

        sigprocmask(SIG_BLOCK, &bloq, &previo);
        pid_t p = fork();
        if (p < 0) { perror("fork"); sigprocmask(SIG_SETMASK, &previo, NULL); break; }

        if (p == 0) {
            /* HIJO */
            num_hijos = 0;   /* heredo la lista del padre: no son mis hijos */
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = al_terminar;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGTERM, &sa, NULL);               /* instalo el manejador */
            sigprocmask(SIG_SETMASK, &previo, NULL);     /* ahora si acepto señales */

            close(ida[k][1]);
            close(vuelta[k][0]);
            for (int j = 0; j < k; j++) close(vuelta[j][0]);

            char buf[256];
            ssize_t n = read(ida[k][0], buf, sizeof(buf) - 1);
            close(ida[k][0]);
            if (n < 0) n = 0;
            buf[n] = '\0';
            srand(getpid());

            Reporte r = meeseeks_arbol(buf, D_hijo, N + 1, k + 1);
            enviar(vuelta[k][1], &r, sizeof(r));
            close(vuelta[k][1]);
            exit(r.resuelto ? EXIT_SUCCESS : EXIT_FAILURE);
        }

        /* PADRE */
        mis_hijos[num_hijos++] = p;                      /* anoto al hijo ANTES de aceptar señales */
        sigprocmask(SIG_SETMASK, &previo, NULL);
        creados++;
        close(ida[k][0]);
        enviar(ida[k][1], solicitud, strlen(solicitud));
        close(ida[k][1]);
        close(vuelta[k][1]);
    }

    /* Vigila los pipes de vuelta de TODOS los hijos a la vez */
    struct pollfd fds[5];
    int vivo[5];   /* 1 = hijo que todavia no ha reportado */
    for (int k = 0; k < creados; k++) {
        fds[k].fd = vuelta[k][0];
        fds[k].events = POLLIN;
        vivo[k] = 1;
    }

    Reporte final = {0, 0, 0, 0, 0};
    int abiertos = creados;
    while (abiertos > 0) {
        if (poll(fds, creados, -1) < 0) {        /* duerme hasta que algun pipe tenga algo */
            if (errno == EINTR) continue;
            break;
        }
        for (int k = 0; k < creados; k++) {
            if (fds[k].fd < 0) continue;         /* poll ignora fds negativos */
            if (!(fds[k].revents & (POLLIN | POLLHUP))) continue;

            Reporte r;
            ssize_t n = read(fds[k].fd, &r, sizeof(r));
            if (n == (ssize_t)sizeof(r)) {
                vivo[k] = 0;                     /* este ya reporto */
                if (r.resuelto && !final.resuelto) {
                    final = r;                   /* primer exito: se lo aviso a los hermanos */
                    for (int j = 0; j < creados; j++)
                        if (vivo[j]) kill(mis_hijos[j], SIGTERM);
                }
            } else {                             /* EOF: el hijo cerro su extremo */
                close(fds[k].fd);
                fds[k].fd = -1;
                vivo[k] = 0;
                abiertos--;
            }
        }
    }
    while (wait(NULL) > 0);   /* recoge a todos: no quedan zombies */
    num_hijos = 0;
    return final;
}

/* Un Meeseeks del caso A: saluda, tira el dado y, si falla, pide ayuda.
   Devuelve el reporte de su subarbol. */
static Reporte meeseeks_arbol(const char *solicitud, double D, int N, int i) {
     registrar(N);
    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d) D=%.1f\n",
           getpid(), getppid(), N, i, D);
    fflush(stdout);

    usleep((useconds_t)(rand01() * ESPERA_MAX_US));   /* simula el tiempo de trabajo */

    if (D > 85.0 || rand01() * 100.0 < D * NERF_DADO || N >= MAX_NIVEL) {
        printf("[pid %d] Resolvi la tarea\n", getpid());
        printf("[pid %d, ppid %d] Mision cumplida. Gracias por servirte, adios!\n", getpid(), getppid());
        fflush(stdout);
        Reporte r = {1, getpid(), getppid(), N, i};
        return r;
    }
    Reporte r = crear_hijos(solicitud, D, N);
    if (r.resuelto) {   /* un descendiente resolvio: yo tambien me despido */
        printf("[pid %d, ppid %d] Mision cumplida. Gracias por servirte, adios!\n", getpid(), getppid());
        fflush(stdout);
    }
    return r;
}

/* ---------- Meeseeks y Box ---------- */

static void saludar(int N, int i) {
     registrar(N);
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



    iniciar_compartido();
    while (1) {   /* Box acepta solicitudes hasta que el usuario se despida */
        printf("Box> ");
        fflush(stdout);   /* evita texto duplicado en los hijos por el buffer */

        if (fgets(linea, sizeof(linea), stdin) == NULL) break;
        linea[strcspn(linea, "\n")] = '\0';   /* quita el salto de linea */

        if (strcmp(linea, "adios") == 0) break;   /* nuestra despedida */
        if (linea[0] == '\0') continue;

        reiniciar_contadores();
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
        printf("Meeseeks creados: %d | nivel maximo: %d\n", comp->total, comp->max_nivel);
        for (int n = 1; n <= comp->max_nivel && n < MAX_NIV_STATS; n++)
            printf("  nivel %d: %d Meeseeks\n", n, comp->por_nivel[n]);
    }

    printf("Box terminó.\n");
    sem_destroy(&comp->mutex);
    munmap(comp, sizeof(Compartido));
    return 0;
}