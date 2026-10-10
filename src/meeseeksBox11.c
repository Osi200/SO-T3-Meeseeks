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
#include <ctype.h>       /* isdigit, para el evaluador */
#include <errno.h>       /* errores de exec y poll */
#include <poll.h>        /* poll: vigilar varios pipes a la vez */
#include <signal.h>      /* sigaction, kill, SIGTERM */
#include <sys/mman.h>    /* mmap: memoria compartida */
#include <semaphore.h>   /* sem_t, sem_init, sem_wait, sem_post */

#define MAX_PROCS_DEF   60    /* tope de Meeseeks vivos a la vez (configurable por argumento) */
#define CAOS_SEG_DEF    300   /* caos planetario: 5 minutos (configurable por argumento) */
#define AGING           5.0   /* cuanto sube D por nivel (la tarea se simplifica) */
#define NERF_DADO       0.1   /* baja la probabilidad del dado */
#define MAX_NIV_STATS   64    /* niveles que se contabilizan por separado */

/* Hijos vivos de ESTE proceso: el manejador de SIGTERM los necesita */
static pid_t mis_hijos[5];
static volatile int num_hijos = 0;

/* Mensaje que viaja por el pipe de vuelta: quien resolvio, donde esta en el
   arbol y cuanto se durmio en la cadena raiz -> resolvedor */
typedef struct {
    int resuelto;          /* 1 = alguien del subarbol resolvio */
    int pid, ppid, N, i;   /* datos del Meeseeks que resolvio */
    double espera_ruta;    /* segundos de simulacion en la ruta ganadora */
} Reporte;

/* Estructura en memoria compartida: todos los Meeseeks ven la misma */
typedef struct {
    sem_t mutex;                    /* semaforo: solo uno a la vez toca los contadores */
    int total;                      /* Meeseeks creados para la solicitud actual */
    int max_nivel;                  /* nivel mas profundo alcanzado */
    int por_nivel[MAX_NIV_STATS];   /* cuantos Meeseeks hay en cada nivel */
    int vivos;                      /* Meeseeks vivos (o con cupo reservado) */
    int max_procs;                  /* tope configurado */
    double espera_final;            /* espera de la ruta ganadora, la escribe el raiz */
} Compartido;

static Compartido *comp;

/* Se llama UNA vez en Box, antes de cualquier fork: los hijos heredan el mapeo */
static void iniciar_compartido(void) {
    comp = mmap(NULL, sizeof(Compartido), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);   /* MAP_SHARED: el fork NO la copia, la comparte */
    if (comp == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }
    sem_init(&comp->mutex, 1, 1);   /* 2do arg 1 = entre procesos; valor inicial 1 = libre */
}

/* Contadores en cero antes de cada solicitud nueva */
static void reiniciar_contadores(void) {
    comp->total = 0;
    comp->max_nivel = 0;
    comp->vivos = 1;            /* el Meeseeks raiz */
    comp->espera_final = 0;
    memset(comp->por_nivel, 0, sizeof(comp->por_nivel));
}

/* Bloquea SIGTERM: si el manejador hiciera _exit con el semaforo tomado, nadie
   mas podria entrar a la seccion critica (deadlock) */
static void bloquear_term(sigset_t *previo) {
    sigset_t bloq;
    sigemptyset(&bloq);
    sigaddset(&bloq, SIGTERM);
    sigprocmask(SIG_BLOCK, &bloq, previo);
}

/* Cada Meeseeks se anota al nacer (los vivos ya se reservaron con reservar_hijos) */
static void registrar(int N) {
    sigset_t previo;
    bloquear_term(&previo);
    sem_wait(&comp->mutex);              /* P: entra a la seccion critica */
    comp->total++;
    if (N > comp->max_nivel) comp->max_nivel = N;
    if (N < MAX_NIV_STATS) comp->por_nivel[N]++;
    sem_post(&comp->mutex);              /* V: sale */
    sigprocmask(SIG_SETMASK, &previo, NULL);
}

/* Cada Meeseeks se da de baja al desaparecer, una sola vez por proceso */
static volatile sig_atomic_t ya_desregistrado = 0;
static volatile sig_atomic_t despedido = 0;   /* 1 = ya imprimi mi despedida */

static void desregistrar(void) {
    if (ya_desregistrado) return;
    ya_desregistrado = 1;
    sigset_t previo;
    bloquear_term(&previo);
    sem_wait(&comp->mutex);
    comp->vivos--;
    sem_post(&comp->mutex);
    sigprocmask(SIG_SETMASK, &previo, NULL);
}

/* Pide cupo para crear n hijos. Devuelve 1 si hay lugar (tope de procesos). */
static int reservar_hijos(int n) {
    sigset_t previo;
    bloquear_term(&previo);
    sem_wait(&comp->mutex);
    int ok = (comp->vivos + n <= comp->max_procs);
    if (ok) comp->vivos += n;   /* reserva ANTES del fork: dos padres no compiten por el ultimo lugar */
    sem_post(&comp->mutex);
    sigprocmask(SIG_SETMASK, &previo, NULL);
    return ok;
}

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

/* Configuracion que Box ajusta antes de cada solicitud; los Meeseeks la heredan con el fork */
static int    modo_dif = 0;      /* 0 aleatoria, 1 fija, 2 preguntar en cada solicitud */
static double dif_fija = 50.0;
static double dif_actual = -1;   /* dificultad de la solicitud en curso; negativo = al azar */
static int    modo_t = 0;        /* 0 aleatorio (0.5 a 5 s), 1 fijo */
static double t_fijo = 1.0;

/* Dificultad con la que arranca el Meeseeks raiz */
static double dificultad_inicial(void) {
    if (dif_actual < 0) return rand() % 101;   /* uniforme 0..100 */
    return dif_actual;
}

/* Simula el tiempo de trabajo y devuelve cuanto durmio (para restarlo del tiempo real) */
static double esperar_trabajo(void) {
    double s = (modo_t == 0) ? 0.5 + rand01() * 4.5 : t_fijo;
    usleep((useconds_t)(s * 1e6));
    return s;
}

/* Se ejecuta cuando llega SIGTERM: avisa a sus hijos, espera que se despidan,
   se despide y desaparece */
static void al_terminar(int sig) {
    (void)sig;
    for (int k = 0; k < num_hijos; k++) kill(mis_hijos[k], SIGTERM);
    while (wait(NULL) > 0);
    if (!despedido) {                 /* si ya me despedi como resolvedor, no repito */
        despedido = 1;
        char msg[128];
        int n = snprintf(msg, sizeof(msg),
            "[pid %d, ppid %d] Me piden terminar. Gracias por servirte, adios!\n",
            getpid(), getppid());
        ssize_t w = write(STDOUT_FILENO, msg, n);
        (void)w;
    }
    desregistrar();
    _exit(EXIT_FAILURE);
}

/* Crea los hijos (todos juntos, en paralelo), les manda la solicitud por el pipe
   de ida y recoge sus reportes por el pipe de vuelta con poll.
   Al primer exito avisa a los hermanos con SIGTERM. Devuelve el reporte del subarbol. */
static Reporte crear_hijos(const char *solicitud, double D, int N) {
    int hijos = (D <= 45.0) ? 5 : 2;           /* regla del enunciado */
    double D_hijo = (D == 0) ? 0 : D + AGING;  /* aging: 0 sigue imposible */
    if (D_hijo > 100.0) D_hijo = 100.0;

    int ida[5][2], vuelta[5][2];   /* ida: padre->hijo, vuelta: hijo->padre */
    int creados = 0;

    sigset_t bloq, previo;
    sigemptyset(&bloq);
    sigaddset(&bloq, SIGTERM);
    num_hijos = 0;

    for (int k = 0; k < hijos; k++) {
        /* Los pipes se crean ANTES del fork para que el hijo los herede */
        if (pipe(ida[k]) < 0 || pipe(vuelta[k]) < 0) { perror("pipe"); break; }

        /* SIGTERM bloqueado alrededor del fork: ninguna señal llega a un hijo
           que todavia no tiene su manejador ni su lista de hijos en orden */
        sigprocmask(SIG_BLOCK, &bloq, &previo);
        pid_t p = fork();
        if (p < 0) { perror("fork"); sigprocmask(SIG_SETMASK, &previo, NULL); break; }

        if (p == 0) {
            /* HIJO */
            num_hijos = 0;   /* la lista heredada es de mi padre, no mia */
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = al_terminar;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGTERM, &sa, NULL);
            sigprocmask(SIG_SETMASK, &previo, NULL);   /* ahora si acepto señales */

            close(ida[k][1]);      /* no escribe en la ida */
            close(vuelta[k][0]);   /* no lee de la vuelta */
            for (int j = 0; j < k; j++) close(vuelta[j][0]);   /* lecturas de hermanos anteriores */

            char buf[256];
            ssize_t n = read(ida[k][0], buf, sizeof(buf) - 1);   /* recibe la solicitud por pipe */
            close(ida[k][0]);
            if (n < 0) n = 0;
            buf[n] = '\0';
            srand(getpid());   /* si no, todos los hermanos tiran el mismo dado */

            Reporte r = meeseeks_arbol(buf, D_hijo, N + 1, k + 1);
            enviar(vuelta[k][1], &r, sizeof(r));   /* reporta al padre */
            close(vuelta[k][1]);
            desregistrar();
            exit(r.resuelto ? EXIT_SUCCESS : EXIT_FAILURE);   /* el hijo nunca vuelve al codigo del padre */
        }

        /* PADRE */
        mis_hijos[num_hijos++] = p;   /* anoto al hijo ANTES de aceptar señales */
        sigprocmask(SIG_SETMASK, &previo, NULL);
        creados++;
        close(ida[k][0]);
        enviar(ida[k][1], solicitud, strlen(solicitud));
        close(ida[k][1]);      /* cerrar avisa "fin del mensaje" */
        close(vuelta[k][1]);   /* el padre solo lee de la vuelta */
    }

    /* Vigila los pipes de vuelta de TODOS los hijos a la vez */
    struct pollfd fds[5];
    int vivo[5];   /* 1 = hijo que todavia no ha reportado */
    for (int k = 0; k < creados; k++) {
        fds[k].fd = vuelta[k][0];
        fds[k].events = POLLIN;
        vivo[k] = 1;
    }

    Reporte final = {0, 0, 0, 0, 0, 0.0};
    int abiertos = creados;
    while (abiertos > 0) {
        if (poll(fds, creados, -1) < 0) {   /* duerme hasta que algun pipe tenga algo */
            if (errno == EINTR) continue;
            break;
        }
        for (int k = 0; k < creados; k++) {
            if (fds[k].fd < 0) continue;   /* poll ignora fds negativos */
            if (!(fds[k].revents & (POLLIN | POLLHUP))) continue;

            Reporte r;
            ssize_t n = read(fds[k].fd, &r, sizeof(r));
            if (n == (ssize_t)sizeof(r)) {
                vivo[k] = 0;   /* este ya reporto */
                if (r.resuelto && !final.resuelto) {
                    final = r;   /* primer exito: se lo aviso a los hermanos */
                    for (int j = 0; j < creados; j++)
                        if (vivo[j]) kill(mis_hijos[j], SIGTERM);
                }
            } else {   /* EOF: el hijo cerro su extremo */
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

/* Un Meeseeks del caso A: saluda, trabaja, tira el dado y, si falla, pide ayuda.
   Devuelve el reporte de su subarbol. */
static Reporte meeseeks_arbol(const char *solicitud, double D, int N, int i) {
    registrar(N);
    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d) D=%.1f\n",
           getpid(), getppid(), N, i, D);
    fflush(stdout);   /* vaciar el buffer antes de cualquier fork */

    double mi_espera = esperar_trabajo();   /* simula el tiempo de trabajo */
    int hijos_necesarios = (D <= 45.0) ? 5 : 2;

    if (D > 85.0 || rand01() * 100.0 < D * NERF_DADO) {
        despedido = 1;
        printf("[pid %d] Resolvi la tarea\n", getpid());
        printf("[pid %d, ppid %d] Mision cumplida. Gracias por servirte, adios!\n", getpid(), getppid());
        fflush(stdout);
        Reporte r = {1, getpid(), getppid(), N, i, mi_espera};
        return r;
    }

    if (!reservar_hijos(hijos_necesarios)) {
        /* Sin cupo: no puede pedir ayuda. Duerme sin gastar CPU hasta que
           llegue SIGTERM (el caso imposible espera el caos planetario) */
        pause();
        Reporte vacio = {0, 0, 0, 0, 0, 0.0};
        return vacio;
    }

    Reporte r = crear_hijos(solicitud, D, N);
    if (r.resuelto) {   /* un descendiente resolvio: yo tambien me despido */
        r.espera_ruta += mi_espera;   /* sumo mi espera a la ruta ganadora */
        despedido = 1;
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

/* Instala al_terminar como respuesta a SIGTERM (para el Meeseeks raiz) */
static void instalar_manejador(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = al_terminar;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
}

/* Lo que hace un Meeseeks raiz: saluda, atiende y desaparece */
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
        fflush(stdout);
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

    /* Caso A: solicitud de texto */
    srand(getpid() ^ time(NULL));
    double D = dificultad_inicial();

    instalar_manejador();   /* el raiz tambien debe poder despedirse y propagar */
    Reporte r = meeseeks_arbol(solicitud, D, N, i);
    if (r.resuelto) {
        comp->espera_final = r.espera_ruta;   /* Box la lee para restarla del tiempo total */
        printf("[pid %d, ppid %d] Tarea completada. La resolvio el Meeseeks (%d, %d, %d, %d)\n",
               getpid(), getppid(), r.pid, r.ppid, r.N, r.i);
        desregistrar();
        exit(EXIT_SUCCESS);
    }
    printf("[pid %d, ppid %d] No se pudo resolver la tarea\n", getpid(), getppid());
    desregistrar();
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    /* uso: ./box [tope_procesos] [segundos_caos] */
    int max_procs = MAX_PROCS_DEF;
    int caos_seg  = CAOS_SEG_DEF;
    if (argc > 1) max_procs = atoi(argv[1]);
    if (argc > 2) caos_seg  = atoi(argv[2]);
    if (max_procs < 1) max_procs = MAX_PROCS_DEF;   /* valores invalidos: usa el default */
    if (caos_seg  < 1) caos_seg  = CAOS_SEG_DEF;

    char linea[256];   /* lo que escribe el usuario */

    iniciar_compartido();   /* antes de cualquier fork: los hijos heredan el mapeo */
    comp->max_procs = max_procs;

    /* Historial de solicitudes para el informe final */
    typedef struct {
        char texto[256];
        double duracion, duracion_real;
        int completada, caos, meeseeks;
    } Registro;
    Registro hist[256];
    int n_hist = 0;

    while (1) {
        printf("Box> ");
        fflush(stdout);

        if (fgets(linea, sizeof(linea), stdin) == NULL) break;
        linea[strcspn(linea, "\n")] = '\0';   /* quita el salto de linea */

        if (strcmp(linea, "adios") == 0) break;
        if (linea[0] == '\0') continue;

        /* Comandos de configuracion: no crean Meeseeks */
        if (strncmp(linea, "dif ", 4) == 0) {
            const char *arg = linea + 4;
            if (strcmp(arg, "aleatoria") == 0) { modo_dif = 0; printf("Dificultad: aleatoria\n"); }
            else if (strcmp(arg, "preguntar") == 0) { modo_dif = 2; printf("Dificultad: se preguntara en cada solicitud\n"); }
            else {
                char *fin;
                double v = strtod(arg, &fin);
                if (fin == arg || v < 0 || v > 100) printf("Valor invalido: usa aleatoria, preguntar o un numero de 0 a 100\n");
                else { modo_dif = 1; dif_fija = v; printf("Dificultad fija: %.1f\n", v); }
            }
            continue;
        }
        if (strncmp(linea, "tiempo ", 7) == 0) {
            const char *arg = linea + 7;
            if (strcmp(arg, "aleatorio") == 0) { modo_t = 0; printf("Tiempo: aleatorio 0.5 a 5 s\n"); }
            else {
                char *fin;
                double v = strtod(arg, &fin);
                if (fin == arg || v < 0) printf("Valor invalido: usa aleatorio o segundos (ej. 0.2)\n");
                else { modo_t = 1; t_fijo = v; printf("Tiempo fijo: %.2f s\n", v); }
            }
            continue;
        }

        /* Decide la dificultad de esta solicitud (el raiz la hereda con el fork) */
        if (modo_dif == 0) dif_actual = -1;
        else if (modo_dif == 1) dif_actual = dif_fija;
        else {
            char resp[64];
            printf("Dificultad de esta solicitud (0 a 100): ");
            fflush(stdout);
            if (fgets(resp, sizeof(resp), stdin) == NULL) break;
            double v = atof(resp);
            dif_actual = (v < 0) ? 0 : (v > 100 ? 100 : v);
        }

        reiniciar_contadores();

        double t0 = ahora();
        fflush(stdout);
        pid_t pid = fork();   /* aqui nace el primer Meeseeks */

        if (pid < 0) {
            perror("fork");
            continue;
        }
        if (pid == 0) meeseeks(linea, 1, 1);   /* hijo: N=1, i=1 */

        /* Espera con limite de tiempo: si se cumple, caos planetario */
        int estado = 0, ok = 0, caos = 0;
        double limite = t0 + caos_seg;
        while (1) {
            pid_t r = waitpid(pid, &estado, WNOHANG);   /* no bloquea */
            if (r == pid) {
                ok = WIFEXITED(estado) && WEXITSTATUS(estado) == 0;
                break;
            }
            if (ahora() > limite) {
                printf("CAOS PLANETARIO: se acabo el tiempo (%d s). Terminando todo.\n", caos_seg);
                fflush(stdout);
                kill(pid, SIGTERM);        /* la cascada baja por todo el arbol */
                waitpid(pid, &estado, 0);  /* espera a que el raiz termine de despedirse */
                caos = 1;
                break;
            }
            usleep(20000);   /* revisa cada 20 ms sin gastar CPU */
        }

        double dur = ahora() - t0;
        double real = dur - comp->espera_final;   /* tiempo sin las esperas simuladas */
        if (real < 0) real = 0;

        if (n_hist < 256) {
            Registro *g = &hist[n_hist++];
            snprintf(g->texto, sizeof(g->texto), "%s", linea);
            g->duracion = dur;
            g->duracion_real = real;
            g->completada = ok && !caos;
            g->caos = caos;
            g->meeseeks = comp->total;
        }

        printf("%s | Duración: %.4f s | sin esperas: %.4f s\n",
               caos ? "No completada (caos planetario)" : (ok ? "Completada" : "No completada"),
               dur, real);
        printf("Meeseeks creados: %d | nivel maximo: %d\n", comp->total, comp->max_nivel);
        for (int n = 1; n <= comp->max_nivel && n < MAX_NIV_STATS; n++)
            printf("  nivel %d: %d Meeseeks\n", n, comp->por_nivel[n]);
    }

    /* Informe final */
    printf("\n===== INFORME FINAL =====\n");
    int comp_ok = 0;
    for (int k = 0; k < n_hist; k++) {
        Registro *g = &hist[k];
        if (g->completada) comp_ok++;
        printf("%2d. \"%s\" | %s | total %.3f s | real (sin esperas) %.3f s | Meeseeks: %d\n",
               k + 1, g->texto,
               g->completada ? "completada" : (g->caos ? "caos planetario" : "no completada"),
               g->duracion, g->duracion_real, g->meeseeks);
    }
    printf("Solicitudes: %d | completadas: %d | no completadas: %d\n",
           n_hist, comp_ok, n_hist - comp_ok);

    printf("Box terminó.\n");
    sem_destroy(&comp->mutex);
    munmap(comp, sizeof(Compartido));
    return 0;
}