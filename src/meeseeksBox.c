/*
 * Instituto Tecnológico de Costa Rica
 * Escuela de Ingeniería en Computación
 * IC-6600 Principios de Sistemas Operativos
 * Tarea 3 - Mr. Meeseeks
 * Estudiante: Oscar Andrés Bezara Pérez, Carné 2024075088
 * Fecha:  9 de octubre 2026
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

/* Como la tarea evalua tiempos, es importante que estos valores se mantengan estables, asi que que tenga un reloj independiente al de la computadora. */
static double ahora(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Lo que hace un Meeseeks: saluda, atiende y desaparece */
static void meeseeks(const char *solicitud, int N, int i) {

    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee!!!! (%d, %d, %d, %d)\n", getpid(), getppid(), N, i);

    printf("[pid %d, ppid %d] Yes sireeee!! Atendiendo: %s\n", getpid(), getppid(), solicitud);

    exit(EXIT_SUCCESS);  /* Aunque un Meeseks pueda llamar mas Meeseeks, un meeseek nunca va a recibir solicitudes nuevas, solo la box del principio */
}

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
            perror("fork");  /*Logica de fork ya vista*/
            continue;
        }
        if (pid == 0) meeseeks(linea, 1, 1);  /*Proceso hijo N=1, i=1: primero del primer nivel */

        int estado;
        waitpid(pid, &estado, 0);  /* Box espera; así no quedan zombies */
        printf("Duración: %.4f s\n", ahora() - t0);
    }

    printf("Box terminó.\n");
    return 0;
}