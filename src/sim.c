/* Simulador del arbol de Meeseeks SIN procesos: solo cuenta cuantos se crearian */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define AGING 1.0     /* factor de envejecimiento de la dificultad */
#define MAX_PROCS 500   /* tope: plan de contingencia para dificultad 0 */
#define NERF_DADO 0.001  /*hacer las probabilidades menores para los pobres meeseeks*/

static int total;       /* Meeseeks creados en esta simulacion */

static double rand01(void) { return rand() / (RAND_MAX + 1.0); }


/* Modo de dificultad, para testing:
 *   0 -> siempre 90 (facil)
 *   1 -> siempre 15 (dificil)
 *   2 -> valor fijo que elija el usuario (FIJO_D)
 *   otro -> aleatoria uniforme en [0,100]  (modo de produccion)
 */
static int modo_dif = -1;      /* por defecto: aleatoria */
static double FIJO_D = 50.0;   /* se usa solo en modo 2 */

static double dificultad(void) {
    switch (modo_dif) {
        case 0:  return 90.0;
        case 1:  return 15.0;
        case 2:  return FIJO_D;
        default: return rand() % 101;   /* uniforme 0..100 */
    }
}

/* Devuelve 1 si algun Meeseeks del subarbol resolvio la tarea */
static int simular(double D) {
    if (total >= MAX_PROCS) return 0;   /* tope alcanzado: nadie mas nace */
    total++;

    if (D > 85.0) return 1;                  /* tarea facil: lo hace solo */



    if (rand01() * 100.0 < D * NERF_DADO) return 1;      /* el dado le alcanzo */


    int hijos;
        if (D <= 45.0) {
            hijos = 5;      /* tarea muy dificil: pide 5 ayudantes */
        } 
        else {
            hijos = 2;      /* tarea media (45 a 85): pide 2 ayudantes */
        }
   
    double D_hijo; 
    if (D == 0) {
            D_hijo = 0;      
        }
        else {
            D_hijo = D + AGING;     
        }
        
                     /* aging: la tarea se simplifica */

    
    if (D_hijo > 100.0) D_hijo = 100.0;

    for (int k = 0; k < hijos; k++)
        if (simular(D_hijo)) return 1;       /* si uno resuelve, los demas paran */
    return 0;
}

int main(int argc, char *argv[]) {
    srand((unsigned)time(NULL));
    /* uso: ./sim [modo] [valor]   ej: ./sim 0   |  ./sim 2 40  |  ./sim */
    if (argc > 1) modo_dif = atoi(argv[1]);
    if (argc > 2) FIJO_D = atof(argv[2]);

    int pruebas = 1000, exitos = 0;
    long suma = 0;

    for (int t = 0; t < pruebas; t++) {
        total = 0;
        double d = dificultad();
        exitos += simular(d);
        suma += total;
    }
    printf("modo=%d | exito: %.1f%% | Meeseeks promedio: %.1f\n",
           modo_dif, 100.0 * exitos / pruebas, (double)suma / pruebas);
    return 0;
}