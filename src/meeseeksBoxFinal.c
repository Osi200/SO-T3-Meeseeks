/*
 * Instituto Tecnológico de Costa Rica
 * Escuela de Ingeniería en Computación
 * IC-6600 Principios de Sistemas Operativos
 * Tarea 3 - Mr. Meeseeks
 * Estudiante: Oscar Andrés Bezara Pérez, Carné 2024075088
 * Fecha: 11 de octubre de 2026
 *
 * Descripción:
 *   Programa Box que recibe solicitudes del usuario y crea Mr. Meeseeks
 *   (procesos con fork) para atenderlas. Soporta tres tipos de solicitud:
 *     - Texto normal:   "quiero un café"  (árbol de Meeseeks, caso A)
 *     - Cálculo:        "calc 3+4*2"      (un solo Meeseeks, caso B)
 *     - Programa:       "exec ls /tmp"    (un solo Meeseeks, caso C)
 *
 * Uso:
 *   ./box [tope_procesos] [segundos_caos] [-v]
 *     tope_procesos : máximo de Meeseeks vivos a la vez (por defecto 60)
 *     segundos_caos : tiempo máximo antes del "caos planetario" (por defecto 300)
 *     -v            : muestra la dificultad D en el saludo (para depurar)
 *
 *   Comandos de configuración dentro de Box:
 *     dif aleatoria | dif preguntar | dif <0-100>
 *     tiempo aleatorio | tiempo <segundos>
 *     adios   (termina Box y muestra el informe final)
 *
 * Compilación:
 *   gcc -Wall -Wextra -pthread -o box src/meeseeksBoxFinal.c
 */

/*------------------------------------------------------------------------------
 * SECCIÓN 1: INCLUDES Y CONSTANTES
 *----------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <ctype.h>       /* isdigit, para el evaluador de operaciones */
#include <errno.h>       /* errno, para distinguir EINTR en poll */
#include <poll.h>        /* poll: vigilar varios pipes a la vez */
#include <signal.h>      /* sigaction, kill, SIGTERM */
#include <sys/mman.h>    /* mmap: memoria compartida entre procesos */
#include <semaphore.h>   /* sem_t, sem_init, sem_wait, sem_post */

#define MAX_PROCS_DEF   60    /* tope de Meeseeks vivos a la vez (configurable por argumento) */
#define CAOS_SEG_DEF    300   /* caos planetario: 5 minutos (configurable por argumento) */
#define AGING           5.0   /* cuánto sube D por nivel (la tarea se simplifica) */
#define NERF_DADO       0.1   /* factor que reduce la probabilidad del dado */
#define MAX_NIV_STATS   64    /* niveles que se contabilizan por separado */
#define MAX_HISTORIAL   256   /* solicitudes que recuerda el informe final */

/*------------------------------------------------------------------------------
 * SECCIÓN 2: TIPOS DE DATOS Y VARIABLES GLOBALES
 *----------------------------------------------------------------------------*/

/*
 * Reporte: el mensaje que viaja por el pipe de VUELTA (hijo -> padre).
 *
 * Cuando un Meeseeks resuelve la tarea, o cuando uno de sus descendientes la
 * resuelve, el reporte sube nivel por nivel hasta el Meeseeks original. Así el
 * original sabe QUIÉN resolvió (pid, ppid, N, i) sin usar variables globales,
 * como exige el enunciado.
 */
typedef struct {
    int resuelto;          /* 1 = alguien del subárbol resolvió la tarea */
    int pid, ppid, N, i;   /* datos del Meeseeks que resolvió */
    double espera_ruta;    /* segundos de simulación en la cadena raíz -> resolvedor */
} Reporte;

/*
 * Compartido: estructura que vive en MEMORIA COMPARTIDA.
 *
 * Después de un fork cada proceso tiene su propia copia de las variables, así
 * que un contador global normal no sirve: si un hijo suma 1, el padre no lo
 * ve. Esta estructura se crea con mmap(MAP_SHARED) antes de los fork, por lo
 * que todos los Meeseeks apuntan a la MISMA región física.
 *
 * Como varios procesos la modifican a la vez, TODO acceso pasa por el
 * semáforo "mutex" (exclusión mutua).
 */
typedef struct {
    sem_t mutex;                    /* semáforo: solo uno a la vez toca los contadores */
    int total;                      /* Meeseeks creados para la solicitud actual */
    int max_nivel;                  /* nivel más profundo alcanzado */
    int por_nivel[MAX_NIV_STATS];   /* cuántos Meeseeks hay en cada nivel */
    int vivos;                      /* Meeseeks vivos (o con cupo reservado) */
    int max_procs;                  /* tope configurado */
    double espera_final;            /* espera de la ruta ganadora, la escribe el raíz */
} Compartido;

/* Una entrada del historial que Box usa para el informe final */
typedef struct {
    char texto[256];
    double duracion;        /* duración total medida */
    double duracion_real;   /* duración sin las esperas simuladas */
    int completada;
    int caos;
    int meeseeks;
} Registro;

static Compartido *comp;   /* puntero a la región compartida */

/* Hijos vivos de ESTE proceso: el manejador de SIGTERM los necesita */
static pid_t mis_hijos[5];
static volatile int num_hijos = 0;

/* Banderas por proceso, protegen contra hacer algo dos veces */
static volatile sig_atomic_t ya_desregistrado = 0;   /* 1 = ya me di de baja del contador */
static volatile sig_atomic_t despedido = 0;          /* 1 = ya imprimí mi despedida */

/*
 * Configuración que Box ajusta antes de cada solicitud. Los Meeseeks la
 * heredan con el fork, así que no necesitan memoria compartida para leerla.
 */
static int    modo_dif = 0;       /* 0 aleatoria, 1 fija, 2 preguntar en cada solicitud */
static double dif_fija = 50.0;    /* valor usado cuando modo_dif == 1 */
static double dif_actual = -1;    /* dificultad de la solicitud en curso; negativo = al azar */
static int    modo_t = 0;         /* 0 tiempo aleatorio (0.5 a 5 s), 1 tiempo fijo */
static double t_fijo = 1.0;       /* segundos usados cuando modo_t == 1 */
static int    verbose = 0;        /* 1 = mostrar D en el saludo */

static Reporte meeseeks_arbol(const char *solicitud, double D, int N, int i);

/*------------------------------------------------------------------------------
 * SECCIÓN 3: UTILIDADES GENERALES
 *----------------------------------------------------------------------------*/

/*
 * Reloj monotónico: solo avanza hacia adelante, aunque alguien cambie la hora
 * del sistema (por ejemplo, la sincronización automática de la VM). Como la
 * tarea evalúa tiempos, necesitamos mediciones estables.
 */
static double ahora(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Número aleatorio entre 0 y 1 (sin incluir el 1) */
static double rand01(void) {
    return rand() / (RAND_MAX + 1.0);
}

/* write() con el resultado "usado", para que gcc no avise por ignorarlo */
static void enviar(int fd, const void *datos, size_t n) {
    ssize_t w = write(fd, datos, n);
    (void)w;
}

/*------------------------------------------------------------------------------
 * SECCIÓN 4: MEMORIA COMPARTIDA Y SEMÁFORO
 *----------------------------------------------------------------------------*/

/* Se llama UNA vez en Box, antes de cualquier fork: los hijos heredan el mapeo */
static void iniciar_compartido(void) {
    /* MAP_SHARED: el fork NO copia esta región, la comparte con los hijos */
    comp = mmap(NULL, sizeof(Compartido), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (comp == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    /* 2.º argumento = 1: el semáforo se comparte entre procesos.
       3.er argumento = 1: valor inicial 1, es decir, "libre". */
    sem_init(&comp->mutex, 1, 1);
}

/* Contadores en cero antes de cada solicitud nueva */
static void reiniciar_contadores(void) {
    comp->total = 0;
    comp->max_nivel = 0;
    comp->vivos = 1;            /* el Meeseeks raíz */
    comp->espera_final = 0;
    memset(comp->por_nivel, 0, sizeof(comp->por_nivel));
}

/*
 * Por qué se bloquea SIGTERM mientras se tiene el semáforo:
 *
 * Si llegara un SIGTERM justo cuando un proceso tiene el semáforo tomado, el
 * manejador haría _exit() sin soltarlo. Ningún otro Meeseeks podría entrar
 * nunca más a la sección crítica y todo el árbol quedaría congelado
 * (deadlock). Con la señal bloqueada, el manejador corre solo DESPUÉS de que
 * el semáforo se liberó.
 */
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

    sem_wait(&comp->mutex);              /* P: entra a la sección crítica */
    comp->total++;
    if (N > comp->max_nivel) {
        comp->max_nivel = N;
    }
    if (N < MAX_NIV_STATS) {
        comp->por_nivel[N]++;
    }
    sem_post(&comp->mutex);              /* V: sale de la sección crítica */

    sigprocmask(SIG_SETMASK, &previo, NULL);
}

/* Cada Meeseeks se da de baja al desaparecer, una sola vez por proceso */
static void desregistrar(void) {
    sigset_t previo;

    if (ya_desregistrado) {
        return;
    }
    ya_desregistrado = 1;

    bloquear_term(&previo);
    sem_wait(&comp->mutex);
    comp->vivos--;
    sem_post(&comp->mutex);
    sigprocmask(SIG_SETMASK, &previo, NULL);
}

/*
 * Reserva de cupo (parte del plan de contingencia contra el caos).
 *
 * Antes de crear n hijos, el padre pide permiso: si los vivos más los nuevos
 * superarían el tope, la respuesta es NO y el Meeseeks no puede pedir ayuda.
 *
 * El cupo se reserva ANTES del fork y de forma atómica (dentro del semáforo).
 * Si se reservara después, dos padres podrían ver "queda un lugar" al mismo
 * tiempo y los dos crearían hijos, pasándose del tope.
 *
 * Consecuencia que conviene documentar: la reserva es "todo o nada" (5 o 2
 * hijos de una vez), así que el tope real queda un poco por debajo del
 * configurado, dependiendo de la forma del árbol.
 */
static int reservar_hijos(int n) {
    sigset_t previo;
    int hay_cupo;

    bloquear_term(&previo);
    sem_wait(&comp->mutex);

    if (comp->vivos + n <= comp->max_procs) {
        comp->vivos += n;
        hay_cupo = 1;
    } else {
        hay_cupo = 0;
    }

    sem_post(&comp->mutex);
    sigprocmask(SIG_SETMASK, &previo, NULL);
    return hay_cupo;
}

/*------------------------------------------------------------------------------
 * SECCIÓN 5: EVALUADOR DE OPERACIONES (CASO B)
 *
 * Método: descenso recursivo. Hay UNA función por nivel de precedencia y cada
 * una pide sus operandos a la función del nivel inmediatamente más fuerte.
 * Esa "cascada" es lo que hace que 3+4*2 dé 11 y no 14.
 *
 * De menor a mayor prioridad:
 *   expr_or  ->  expr_xor  ->  expr_and  ->  suma  ->  termino  ->  unario  ->  primario
 *      |             ^             &          + -       * / %      - ~ !      números y ( )
 *----------------------------------------------------------------------------*/

static const char *cur;        /* puntero que recorre el texto: el carácter actual */
static int hubo_error;         /* 1 si algo salió mal */
static const char *msg_error;  /* descripción de lo que salió mal */

static void error(const char *m) {
    hubo_error = 1;
    msg_error = m;
}

/* Avanza el puntero mientras haya espacios, así "3 + 4" equivale a "3+4" */
static void saltar(void) {
    while (*cur == ' ') {
        cur++;
    }
}

static long long expr_or(void);   /* declaración adelantada: los paréntesis la necesitan */

/*
 * Nivel más básico: un número o un paréntesis.
 * Un paréntesis REINICIA la precedencia: lo de adentro se evalúa desde
 * expr_or(), el nivel más bajo, como si fuera una expresión nueva.
 */
static long long primario(void) {
    saltar();

    if (*cur == '(') {
        long long v;
        cur++;
        v = expr_or();
        saltar();
        if (*cur != ')') {
            error("falta ')'");
            return 0;
        }
        cur++;
        return v;
    }

    if (isdigit((unsigned char)*cur)) {
        char *fin;
        long long v = strtoll(cur, &fin, 10);   /* lee el número y avanza cur */
        cur = fin;
        return v;
    }

    error("se esperaba un número");
    return 0;
}

/*
 * Signos pegados a un solo valor: - (negativo), ~ (NOT bit a bit), ! (NOT lógico).
 * Se llama a sí misma (recursión), por eso --5 vale 5: el segundo '-' cambia
 * el signo que había puesto el primero.
 */
static long long unario(void) {
    saltar();

    if (*cur == '-') {
        cur++;
        return -unario();
    }
    if (*cur == '~') {
        cur++;
        return ~unario();
    }
    if (*cur == '!') {
        cur++;
        return !unario();
    }
    return primario();
}

/* Multiplicación, división y módulo. Aquí se protege contra la división entre cero */
static long long termino(void) {
    long long v = unario();

    while (!hubo_error) {
        char op;
        long long d;

        saltar();
        op = *cur;
        if (op != '*' && op != '/' && op != '%') {
            break;
        }
        cur++;
        d = unario();

        if (op == '*') {
            v = v * d;
        } else if (d == 0) {
            error("división entre cero");
            return 0;
        } else if (op == '/') {
            v = v / d;
        } else {
            v = v % d;
        }
    }
    return v;
}

/* Suma y resta */
static long long suma(void) {
    long long v = termino();

    while (!hubo_error) {
        char op;
        long long d;

        saltar();
        op = *cur;
        if (op != '+' && op != '-') {
            break;
        }
        cur++;
        d = termino();

        if (op == '+') {
            v = v + d;
        } else {
            v = v - d;
        }
    }
    return v;
}

/* AND bit a bit */
static long long expr_and(void) {
    long long v = suma();

    while (!hubo_error) {
        saltar();
        if (*cur != '&') {
            break;
        }
        cur++;
        v = v & suma();
    }
    return v;
}

/* XOR bit a bit */
static long long expr_xor(void) {
    long long v = expr_and();

    while (!hubo_error) {
        saltar();
        if (*cur != '^') {
            break;
        }
        cur++;
        v = v ^ expr_and();
    }
    return v;
}

/* OR bit a bit: el nivel más bajo, es el primero que se llama y el último que opera */
static long long expr_or(void) {
    long long v = expr_xor();

    while (!hubo_error) {
        saltar();
        if (*cur != '|') {
            break;
        }
        cur++;
        v = v | expr_xor();
    }
    return v;
}

/* Devuelve 0 si salió bien (resultado en *res), 1 si hubo error */
static int evaluar(const char *texto, long long *res) {
    cur = texto;
    hubo_error = 0;
    *res = expr_or();
    saltar();

    /* Si sobró texto sin consumir, la expresión era inválida (por ejemplo "2 3") */
    if (!hubo_error && *cur != '\0') {
        error("caracteres sobrantes");
    }
    return hubo_error;
}

/*------------------------------------------------------------------------------
 * SECCIÓN 6: DIFICULTAD, TIEMPO Y FÓRMULA DEL ÁRBOL
 *
 * ESCALA: D va de 0 a 100 y es la "probabilidad de que la tarea se pueda
 * resolver". 100 = facilísima, 0 = imposible.
 *
 * FÓRMULA PROPIA DE CADA MEESEEKS (en meeseeks_arbol):
 *   1. Si D > 85, lo resuelve él solo (regla del enunciado).
 *   2. Si no, tira un dado: gana si (número entre 0 y 100) < D * NERF_DADO.
 *      NERF_DADO = 0.1 atenúa el dado. Sin esa atenuación, las tareas
 *      difíciles se resolvían casi siempre en los primeros niveles y el
 *      árbol salía de apenas unos pocos procesos. Con el valor 0.1, la
 *      dificultad se "paga" en niveles de profundidad.
 *   3. Si falla, pide ayuda: 5 hijos si D <= 45, 2 hijos si 45 < D <= 85.
 *
 * AGING (envejecimiento): a cada hijo se le pasa D + AGING. La tarea se
 * "diluye" entre los Meeseeks y se vuelve más fácil con cada nivel, hasta que
 * D supera 85 y algún Meeseeks la resuelve con seguridad. Es la misma idea
 * del aging de planificación: lo que espera mucho gana prioridad.
 *
 * CASO IMPOSIBLE: si D == 0, el aging NO lo sube (0 se queda en 0), y el dado
 * nunca acierta (ningún número es menor que 0). Por eso la tarea no se
 * resuelve jamás, sin tener que programar un caso especial. Lo que evita el
 * colapso es el tope de procesos más el caos planetario.
 *----------------------------------------------------------------------------*/

/* Dificultad con la que arranca el Meeseeks raíz */
static double dificultad_inicial(void) {
    if (dif_actual < 0) {
        return rand() % 101;   /* uniforme entre 0 y 100 */
    }
    return dif_actual;
}

/* Regla del enunciado: cuántos Meeseeks nuevos crea un Meeseeks que pide ayuda */
static int cantidad_hijos(double D) {
    if (D <= 45.0) {
        return 5;   /* tarea muy difícil: pide 5 ayudantes */
    }
    return 2;       /* tarea media (45 a 85): pide 2 ayudantes */
}

/* Aging: dificultad que reciben los hijos */
static double dificultad_hijo(double D) {
    double D_hijo;

    if (D == 0) {
        D_hijo = 0;            /* imposible: nunca se simplifica */
    } else {
        D_hijo = D + AGING;    /* la tarea se simplifica un poco por nivel */
    }

    if (D_hijo > 100.0) {
        D_hijo = 100.0;        /* 100 es el máximo de la escala */
    }
    return D_hijo;
}

/* Simula el tiempo de trabajo y devuelve cuánto durmió (para restarlo después) */
static double esperar_trabajo(void) {
    double segundos;

    if (modo_t == 0) {
        segundos = 0.5 + rand01() * 4.5;   /* aleatorio entre 0.5 y 5 s */
    } else {
        segundos = t_fijo;                 /* valor elegido por el usuario */
    }

    usleep((useconds_t)(segundos * 1e6));
    return segundos;
}

/*------------------------------------------------------------------------------
 * SECCIÓN 7: TERMINACIÓN EN CASCADA (SEÑALES)
 *
 * Por qué SIGTERM y no otro mecanismo:
 *   Cuando un Meeseeks resuelve la tarea, sus hermanos (que siguen trabajando
 *   o durmiendo) deben enterarse y despedirse. Hay dos opciones: avisarles
 *   por pipe o con una señal. Un pipe obligaría a cada Meeseeks a estar
 *   leyendo mientras trabaja. Una señal, en cambio, interrumpe al proceso
 *   aunque esté dormido en usleep() o en pause().
 *
 * Cómo funciona la cascada:
 *   1. El padre recibe por pipe el primer reporte de éxito.
 *   2. Manda SIGTERM a los hermanos que siguen vivos.
 *   3. Cada hermano, en su manejador (al_terminar), manda SIGTERM a SUS
 *      hijos y ESPERA con wait() a que se despidan antes de despedirse él.
 *   4. Así la despedida va de abajo hacia arriba y nadie queda huérfano
 *      (sin ese wait, un padre moría antes que sus hijos y estos eran
 *      adoptados por init).
 *
 * El mismo mecanismo se usa para el CAOS PLANETARIO: Box manda SIGTERM al
 * Meeseeks raíz y la cascada baja por todo el árbol.
 *
 * Se usa write() y _exit() en vez de printf() y exit() porque solo algunas
 * funciones son seguras dentro de un manejador de señales; _exit tampoco
 * vacía buffers heredados del padre, lo que evitaría texto duplicado.
 *----------------------------------------------------------------------------*/

static void al_terminar(int sig) {
    (void)sig;

    /* Propaga la señal hacia abajo */
    for (int k = 0; k < num_hijos; k++) {
        kill(mis_hijos[k], SIGTERM);
    }

    /* Espera a que los hijos se despidan primero */
    while (wait(NULL) > 0) {
        /* nada: solo recogemos hijos */
    }

    /* Si ya me despedí como resolvedor, no repito la despedida */
    if (!despedido) {
        char msg[128];
        int n;

        despedido = 1;
        n = snprintf(msg, sizeof(msg),
                     "[pid %d, ppid %d] Me piden terminar. ¡Gracias por servirte, adiós!\n",
                     getpid(), getppid());
        enviar(STDOUT_FILENO, msg, n);
    }

    desregistrar();
    _exit(EXIT_FAILURE);
}

/* Instala al_terminar como respuesta a SIGTERM */
static void instalar_manejador(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = al_terminar;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
}

/*------------------------------------------------------------------------------
 * SECCIÓN 8: ÁRBOL DE MEESEEKS (CASO A) CON PIPES
 *
 * Comunicación entre procesos (sin variables globales para mensajes):
 *
 *   PIPE DE IDA (padre -> hijo): el padre le pasa la solicitud al hijo. Se
 *   crea ANTES del fork para que el hijo herede los dos extremos. El padre
 *   escribe y cierra; el hijo lee. Es un mensaje privado por cada hijo.
 *
 *   PIPE DE VUELTA (hijo -> padre): el hijo devuelve su Reporte. Un pipe es
 *   unidireccional, por eso hace falta uno distinto para cada dirección.
 *
 * Regla de oro de los pipes: cada proceso cierra los extremos que no usa. Un
 * read() solo ve el fin de archivo cuando TODOS los extremos de escritura
 * están cerrados; si un hijo dejara abierto un extremo ajeno, el padre
 * esperaría para siempre.
 *
 * El padre vigila los pipes de vuelta de todos sus hijos a la vez con poll(),
 * para reaccionar al PRIMERO que reporte éxito (y no al primero de la lista).
 *----------------------------------------------------------------------------*/

/*
 * Crea los hijos (todos juntos, en paralelo), les manda la solicitud por el
 * pipe de ida y recoge sus reportes por el pipe de vuelta.
 * Al primer éxito avisa a los hermanos con SIGTERM. Devuelve el reporte del
 * subárbol.
 */
static Reporte crear_hijos(const char *solicitud, double D, int N) {
    int hijos = cantidad_hijos(D);
    double D_hijo = dificultad_hijo(D);
    int ida[5][2];      /* ida:    padre -> hijo */
    int vuelta[5][2];   /* vuelta: hijo -> padre */
    int creados = 0;
    sigset_t bloq, previo;

    sigemptyset(&bloq);
    sigaddset(&bloq, SIGTERM);
    num_hijos = 0;

    /*---------------- Creación de los hijos ----------------*/
    for (int k = 0; k < hijos; k++) {
        pid_t p;

        /* Los pipes se crean ANTES del fork para que el hijo los herede */
        if (pipe(ida[k]) < 0 || pipe(vuelta[k]) < 0) {
            perror("pipe");
            break;
        }

        /*
         * SIGTERM bloqueado alrededor del fork: así ninguna señal llega a un
         * hijo recién nacido que todavía no instaló su manejador, ni a un
         * padre que todavía no anotó al hijo en su lista mis_hijos.
         */
        sigprocmask(SIG_BLOCK, &bloq, &previo);
        p = fork();
        if (p < 0) {
            perror("fork");
            sigprocmask(SIG_SETMASK, &previo, NULL);
            break;
        }

        if (p == 0) {
            /*------------ HIJO ------------*/
            char buf[256];
            ssize_t n;
            Reporte r;

            num_hijos = 0;   /* la lista heredada es de mi padre, no mía */
            instalar_manejador();
            sigprocmask(SIG_SETMASK, &previo, NULL);   /* ahora sí acepto señales */

            close(ida[k][1]);      /* no escribo en la ida */
            close(vuelta[k][0]);   /* no leo de la vuelta */
            for (int j = 0; j < k; j++) {
                close(vuelta[j][0]);   /* lecturas de hermanos anteriores que heredé */
            }

            /* Recibo la solicitud por el pipe (y no por la memoria copiada) */
            n = read(ida[k][0], buf, sizeof(buf) - 1);
            close(ida[k][0]);
            if (n < 0) {
                n = 0;
            }
            buf[n] = '\0';

            srand(getpid());   /* si no, todos los hermanos tirarían el mismo dado */

            r = meeseeks_arbol(buf, D_hijo, N + 1, k + 1);
            enviar(vuelta[k][1], &r, sizeof(r));   /* reporto al padre */
            close(vuelta[k][1]);
            desregistrar();

            /* El hijo NUNCA debe volver al código del padre: termina aquí */
            if (r.resuelto) {
                exit(EXIT_SUCCESS);
            }
            exit(EXIT_FAILURE);
        }

        /*------------ PADRE ------------*/
        mis_hijos[num_hijos++] = p;   /* anoto al hijo ANTES de aceptar señales */
        sigprocmask(SIG_SETMASK, &previo, NULL);
        creados++;

        close(ida[k][0]);                                /* no leo de la ida */
        enviar(ida[k][1], solicitud, strlen(solicitud));   /* le mando la solicitud */
        close(ida[k][1]);                                /* cerrar avisa "fin del mensaje" */
        close(vuelta[k][1]);                             /* solo leo de la vuelta */
    }

    /*---------------- Espera de reportes con poll ----------------*/
    struct pollfd fds[5];
    int vivo[5];   /* 1 = hijo que todavía no ha reportado */
    Reporte final = {0, 0, 0, 0, 0, 0.0};
    int abiertos = creados;

    for (int k = 0; k < creados; k++) {
        fds[k].fd = vuelta[k][0];
        fds[k].events = POLLIN;
        vivo[k] = 1;
    }

    while (abiertos > 0) {
        /* Duerme hasta que algún pipe tenga algo que leer */
        if (poll(fds, creados, -1) < 0) {
            if (errno == EINTR) {
                continue;   /* una señal interrumpió poll: reintenta */
            }
            break;
        }

        for (int k = 0; k < creados; k++) {
            Reporte r;
            ssize_t n;

            if (fds[k].fd < 0) {
                continue;   /* poll ignora descriptores negativos: ya cerrado */
            }
            if (!(fds[k].revents & (POLLIN | POLLHUP))) {
                continue;   /* este pipe no tiene novedades */
            }

            n = read(fds[k].fd, &r, sizeof(r));
            if (n == (ssize_t)sizeof(r)) {
                /* Llegó un reporte completo */
                vivo[k] = 0;
                if (r.resuelto && !final.resuelto) {
                    /* Primer éxito: lo guardo y aviso a los hermanos que sigan vivos */
                    final = r;
                    for (int j = 0; j < creados; j++) {
                        if (vivo[j]) {
                            kill(mis_hijos[j], SIGTERM);
                        }
                    }
                }
            } else {
                /* Fin de archivo: el hijo cerró su extremo, no habrá más datos */
                close(fds[k].fd);
                fds[k].fd = -1;
                vivo[k] = 0;
                abiertos--;
            }
        }
    }

    /* Recoge a todos los hijos: no quedan zombies */
    while (wait(NULL) > 0) {
        /* nada: solo recogemos hijos */
    }
    num_hijos = 0;
    return final;
}

/*
 * Un Meeseeks del caso A: saluda, trabaja, tira el dado y, si falla, pide
 * ayuda. Devuelve el reporte de su subárbol.
 *
 * Tiempo "real": cada Meeseeks que duerme suma su espera al reporte mientras
 * este sube por la cadena raíz -> resolvedor (ruta crítica). Como los
 * Meeseeks trabajan en paralelo, sus esperas se solapan y no se pueden
 * restar todas; solo la espera de la cadena ganadora aparece de verdad en el
 * tiempo total.
 */
static Reporte meeseeks_arbol(const char *solicitud, double D, int N, int i) {
    double mi_espera;
    int hijos_necesarios;
    Reporte r;

    registrar(N);

    if (verbose) {
        printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d) D=%.1f\n",
               getpid(), getppid(), N, i, D);
    } else {
        printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d)\n",
               getpid(), getppid(), N, i);
    }
    fflush(stdout);   /* vaciar el buffer antes de cualquier fork */

    mi_espera = esperar_trabajo();   /* simula el tiempo de trabajo */
    hijos_necesarios = cantidad_hijos(D);

    /* ¿Lo resuelvo yo solo? Regla fija (D > 85) o dado atenuado */
    if (D > 85.0 || rand01() * 100.0 < D * NERF_DADO) {
        despedido = 1;
        printf("[pid %d] Resolví la tarea\n", getpid());
        printf("[pid %d, ppid %d] Misión cumplida. ¡Gracias por servirte, adiós!\n",
               getpid(), getppid());
        fflush(stdout);

        r.resuelto = 1;
        r.pid = getpid();
        r.ppid = getppid();
        r.N = N;
        r.i = i;
        r.espera_ruta = mi_espera;
        return r;
    }

    /* No pude solo: pido cupo para crear ayudantes */
    if (!reservar_hijos(hijos_necesarios)) {
        /*
         * Sin cupo: no puedo pedir ayuda. Duermo sin gastar CPU hasta que
         * llegue SIGTERM (de mi padre, porque alguien resolvió, o del caos
         * planetario). Así los Meeseeks sobrantes de una tarea imposible
         * quedan quietos y no consumen procesador.
         */
        pause();
        r.resuelto = 0;
        r.pid = 0;
        r.ppid = 0;
        r.N = 0;
        r.i = 0;
        r.espera_ruta = 0.0;
        return r;
    }

    r = crear_hijos(solicitud, D, N);
    if (r.resuelto) {
        /* Un descendiente resolvió: yo también me despido */
        r.espera_ruta += mi_espera;   /* sumo mi espera a la ruta ganadora */
        despedido = 1;
        printf("[pid %d, ppid %d] Misión cumplida. ¡Gracias por servirte, adiós!\n",
               getpid(), getppid());
        fflush(stdout);
    }
    return r;
}

/*------------------------------------------------------------------------------
 * SECCIÓN 9: LOS TRES TIPOS DE SOLICITUD
 *----------------------------------------------------------------------------*/

/* Saludo de los Meeseeks de los casos B y C (el caso A saluda en meeseeks_arbol) */
static void saludar(int N, int i) {
    registrar(N);
    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d)\n",
           getpid(), getppid(), N, i);
    fflush(stdout);
}

/*
 * Caso B: solicitud de cálculo. La resuelve un solo Meeseeks.
 * El código de salida le dice a Box si funcionó: 0 = completada, 1 = error.
 */
static void caso_calculo(const char *expresion, int N, int i) {
    long long res;

    saludar(N, i);
    if (evaluar(expresion, &res) == 0) {
        printf("[pid %d, ppid %d] Resultado: %lld\n", getpid(), getppid(), res);
        exit(EXIT_SUCCESS);
    }
    printf("[pid %d, ppid %d] Error: %s\n", getpid(), getppid(), msg_error);
    exit(EXIT_FAILURE);
}

/*
 * Caso C: programa externo, por ejemplo "exec ls /tmp".
 *
 * exec REEMPLAZA el proceso entero por el programa nuevo. Si el Meeseeks
 * hiciera exec directamente, desaparecería sin poder reportar nada. Por eso
 * hace su propio fork: el hijo ejecuta el programa y el Meeseeks espera con
 * waitpid para leer el valor de retorno (0 = éxito, otro valor = fallo).
 * Quedan tres niveles de procesos: Box, el Meeseeks y el programa externo.
 */
static void caso_programa(const char *comando, int N, int i) {
    char copia[256];   /* copia porque strtok modifica el texto */
    char *args[32];
    char *tok;
    int n = 0;
    int est;
    pid_t pid_ext;

    saludar(N, i);

    strncpy(copia, comando, sizeof(copia) - 1);
    copia[sizeof(copia) - 1] = '\0';

    /* Parte "ls /tmp" en {"ls", "/tmp", NULL}, el formato que pide execvp */
    tok = strtok(copia, " ");
    while (tok != NULL && n < 31) {
        args[n++] = tok;
        tok = strtok(NULL, " ");
    }
    args[n] = NULL;   /* execvp exige que el arreglo termine en NULL */

    if (n == 0) {
        printf("[pid %d, ppid %d] Error: no indicaste ningún programa\n",
               getpid(), getppid());
        exit(EXIT_FAILURE);
    }

    fflush(stdout);
    pid_ext = fork();
    if (pid_ext < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid_ext == 0) {
        execvp(args[0], args);   /* si funciona, esta línea nunca retorna */
        /* Si llegamos aquí, exec falló (el programa no existe, sin permisos...) */
        fprintf(stderr, "[pid %d] No se pudo ejecutar '%s'\n", getpid(), args[0]);
        _exit(127);              /* 127 = "comando no encontrado" por convención */
    }

    waitpid(pid_ext, &est, 0);

    if (WIFEXITED(est) && WEXITSTATUS(est) == 0) {
        printf("[pid %d, ppid %d] Programa '%s' terminó con éxito (código 0)\n",
               getpid(), getppid(), args[0]);
        exit(EXIT_SUCCESS);
    }

    if (WIFEXITED(est)) {
        printf("[pid %d, ppid %d] Programa '%s' falló (código %d)\n",
               getpid(), getppid(), args[0], WEXITSTATUS(est));
    } else {
        printf("[pid %d, ppid %d] Programa '%s' murió por una señal\n",
               getpid(), getppid(), args[0]);
    }
    exit(EXIT_FAILURE);
}

/*
 * Caso A: solicitud de texto. Este es el Meeseeks RAÍZ del árbol.
 * Toma la dificultad inicial, lanza el árbol y, al final, informa quién
 * resolvió la tarea.
 */
static void caso_texto(const char *solicitud, int N, int i) {
    double D;
    Reporte r;

    srand(getpid() ^ time(NULL));
    D = dificultad_inicial();

    instalar_manejador();   /* el raíz también debe poder despedirse y propagar */
    r = meeseeks_arbol(solicitud, D, N, i);

    if (r.resuelto) {
        comp->espera_final = r.espera_ruta;   /* Box la lee para restarla del tiempo total */
        printf("[pid %d, ppid %d] Tarea completada. La resolvió el Meeseeks (%d, %d, %d, %d)\n",
               getpid(), getppid(), r.pid, r.ppid, r.N, r.i);
        desregistrar();
        exit(EXIT_SUCCESS);
    }

    printf("[pid %d, ppid %d] No se pudo resolver la tarea\n", getpid(), getppid());
    desregistrar();
    exit(EXIT_FAILURE);
}

/*
 * Un Meeseeks raíz: decide qué tipo de solicitud es según su prefijo y la
 * atiende. Nunca retorna: cada caso termina con exit().
 *
 * Se usan prefijos ("calc ", "exec ") y no se intenta adivinar el tipo,
 * porque adivinar es frágil: "2 cafés" tiene un número y no es un cálculo.
 */
static void meeseeks(const char *solicitud, int N, int i) {
    if (strncmp(solicitud, "calc ", 5) == 0) {
        caso_calculo(solicitud + 5, N, i);
    }
    if (strncmp(solicitud, "exec ", 5) == 0) {
        caso_programa(solicitud + 5, N, i);
    }
    caso_texto(solicitud, N, i);
}

/*------------------------------------------------------------------------------
 * SECCIÓN 10: FUNCIONES DE APOYO DE BOX
 *----------------------------------------------------------------------------*/

/*
 * Procesa los comandos de configuración ("dif ..." y "tiempo ...").
 * Devuelve 1 si la línea era un comando (y ya se atendió), 0 si no.
 */
static int procesar_configuracion(const char *linea) {
    if (strncmp(linea, "dif ", 4) == 0) {
        const char *arg = linea + 4;

        if (strcmp(arg, "aleatoria") == 0) {
            modo_dif = 0;
            printf("Dificultad: aleatoria\n");
        } else if (strcmp(arg, "preguntar") == 0) {
            modo_dif = 2;
            printf("Dificultad: se preguntará en cada solicitud\n");
        } else {
            char *fin;
            double v = strtod(arg, &fin);
            if (fin == arg || v < 0 || v > 100) {
                printf("Valor inválido: usa aleatoria, preguntar o un número de 0 a 100\n");
            } else {
                modo_dif = 1;
                dif_fija = v;
                printf("Dificultad fija: %.1f\n", v);
            }
        }
        return 1;
    }

    if (strncmp(linea, "tiempo ", 7) == 0) {
        const char *arg = linea + 7;

        if (strcmp(arg, "aleatorio") == 0) {
            modo_t = 0;
            printf("Tiempo: aleatorio de 0.5 a 5 s\n");
        } else {
            char *fin;
            double v = strtod(arg, &fin);
            if (fin == arg || v < 0) {
                printf("Valor inválido: usa aleatorio o segundos (ej. 0.2)\n");
            } else {
                modo_t = 1;
                t_fijo = v;
                printf("Tiempo fijo: %.2f s\n", v);
            }
        }
        return 1;
    }

    return 0;
}

/*
 * Decide la dificultad de la solicitud que se va a lanzar (el Meeseeks raíz la
 * hereda con el fork). Devuelve 0 si todo bien, 1 si se acabó la entrada.
 */
static int decidir_dificultad(void) {
    char resp[64];
    double v;

    if (modo_dif == 0) {
        dif_actual = -1;           /* al azar: la tira el Meeseeks raíz */
        return 0;
    }
    if (modo_dif == 1) {
        dif_actual = dif_fija;     /* valor fijo elegido por el usuario */
        return 0;
    }

    /* modo_dif == 2: se pregunta en cada solicitud */
    printf("Dificultad de esta solicitud (0 a 100): ");
    fflush(stdout);
    if (fgets(resp, sizeof(resp), stdin) == NULL) {
        return 1;
    }

    v = atof(resp);
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    dif_actual = v;
    return 0;
}

/*
 * Espera al Meeseeks raíz con un límite de tiempo. Si se cumple, se declara el
 * CAOS PLANETARIO: se manda SIGTERM al raíz y la cascada de señales termina
 * todo el árbol (sección 7).
 *
 * Se usa waitpid con WNOHANG (no bloquea) y se revisa cada 20 ms: así Box
 * puede vigilar el reloj sin gastar CPU.
 */
static void esperar_con_limite(pid_t pid, double t0, int caos_seg, int *ok, int *caos) {
    int estado = 0;
    double limite = t0 + caos_seg;

    *ok = 0;
    *caos = 0;

    while (1) {
        pid_t r = waitpid(pid, &estado, WNOHANG);

        if (r == pid) {
            /* El Meeseeks raíz terminó: éxito si salió normal con código 0 */
            if (WIFEXITED(estado) && WEXITSTATUS(estado) == 0) {
                *ok = 1;
            }
            return;
        }

        if (ahora() > limite) {
            printf("CAOS PLANETARIO: se acabó el tiempo (%d s). Terminando todo.\n", caos_seg);
            fflush(stdout);
            kill(pid, SIGTERM);        /* la cascada baja por todo el árbol */
            waitpid(pid, &estado, 0);  /* espera a que el raíz termine de despedirse */
            *caos = 1;
            return;
        }

        usleep(20000);   /* revisa cada 20 ms sin gastar CPU */
    }
}

/*------------------------------------------------------------------------------
 * SECCIÓN 11: PROGRAMA PRINCIPAL (BOX)
 *----------------------------------------------------------------------------*/

int main(int argc, char *argv[]) {
    int max_procs = MAX_PROCS_DEF;
    int caos_seg = CAOS_SEG_DEF;
    char linea[256];             /* lo que escribe el usuario */
    Registro hist[MAX_HISTORIAL];
    int n_hist = 0;

    /*---------------- Argumentos: ./box [tope] [segundos_caos] [-v] ----------------*/
    if (argc > 1) {
        max_procs = atoi(argv[1]);
    }
    if (argc > 2) {
        caos_seg = atoi(argv[2]);
    }
    if (argc > 3 && strcmp(argv[3], "-v") == 0) {
        verbose = 1;
    }
    if (max_procs < 1) {
        max_procs = MAX_PROCS_DEF;   /* valor inválido: usa el predeterminado */
    }
    if (caos_seg < 1) {
        caos_seg = CAOS_SEG_DEF;
    }

    iniciar_compartido();   /* antes de cualquier fork: los hijos heredan el mapeo */
    comp->max_procs = max_procs;

    /*---------------- Ciclo principal: una solicitud tras otra ----------------*/
    while (1) {
        double t0, dur, real;
        int ok, caos;
        pid_t pid;

        printf("Box> ");
        fflush(stdout);

        if (fgets(linea, sizeof(linea), stdin) == NULL) {
            break;   /* fin de la entrada (Ctrl+D) */
        }
        linea[strcspn(linea, "\n")] = '\0';   /* quita el salto de línea */

        if (strcmp(linea, "adios") == 0) {
            break;   /* el usuario se despide */
        }
        if (linea[0] == '\0') {
            continue;   /* solo presionó Enter: no se crea ningún Meeseeks */
        }
        if (procesar_configuracion(linea)) {
            continue;   /* era un comando de configuración */
        }
        if (decidir_dificultad() != 0) {
            break;
        }

        reiniciar_contadores();

        t0 = ahora();
        fflush(stdout);   /* evita que el hijo herede texto pendiente y lo duplique */
        pid = fork();     /* aquí nace el primer Meeseeks */

        if (pid < 0) {
            perror("fork");
            continue;
        }
        if (pid == 0) {
            meeseeks(linea, 1, 1);   /* hijo: nivel N=1, instancia i=1 */
        }

        /* Box espera al Meeseeks raíz, con límite de tiempo */
        esperar_con_limite(pid, t0, caos_seg, &ok, &caos);

        dur = ahora() - t0;
        real = dur - comp->espera_final;   /* tiempo sin las esperas simuladas */
        if (real < 0) {
            real = 0;
        }

        /* Guarda la solicitud en el historial para el informe final */
        if (n_hist < MAX_HISTORIAL) {
            Registro *g = &hist[n_hist];
            n_hist++;
            snprintf(g->texto, sizeof(g->texto), "%s", linea);
            g->duracion = dur;
            g->duracion_real = real;
            g->completada = (ok && !caos);
            g->caos = caos;
            g->meeseeks = comp->total;
        }

        /* Resumen de esta solicitud */
        if (caos) {
            printf("No completada (caos planetario)");
        } else if (ok) {
            printf("Completada");
        } else {
            printf("No completada");
        }
        printf(" | Duración: %.4f s | sin esperas: %.4f s\n", dur, real);

        printf("Meeseeks creados: %d | nivel máximo: %d\n", comp->total, comp->max_nivel);
        for (int n = 1; n <= comp->max_nivel && n < MAX_NIV_STATS; n++) {
            printf("  nivel %d: %d Meeseeks\n", n, comp->por_nivel[n]);
        }
    }

    /*---------------- Informe final ----------------*/
    printf("\n===== INFORME FINAL =====\n");
    int completadas = 0;

    for (int k = 0; k < n_hist; k++) {
        Registro *g = &hist[k];
        const char *estado;

        if (g->completada) {
            completadas++;
            estado = "completada";
        } else if (g->caos) {
            estado = "caos planetario";
        } else {
            estado = "no completada";
        }

        printf("%2d. \"%s\" | %s | total %.3f s | real (sin esperas) %.3f s | Meeseeks: %d\n",
               k + 1, g->texto, estado, g->duracion, g->duracion_real, g->meeseeks);
    }
    printf("Solicitudes: %d | completadas: %d | no completadas: %d\n",
           n_hist, completadas, n_hist - completadas);

    /*---------------- Limpieza ----------------*/
    printf("Box terminó.\n");
    sem_destroy(&comp->mutex);
    munmap(comp, sizeof(Compartido));
    return 0;
}