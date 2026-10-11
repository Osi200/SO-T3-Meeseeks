#!/bin/bash
# Experimento 1: dificultad fija vs cantidad de Meeseeks y tiempo
# Uso: bash experimentos/exp1.sh
mkdir -p experimentos/resultados
for D in 90 60 40 15 5; do
  for rep in 1 2 3 4 5; do
    # para que los test no sean tan lentos pero con la forma del arbol
    printf "tiempo 0.2\ndif $D\nhola\nadios\n" | ./box 200 60 \
      > experimentos/resultados/exp1_D${D}_r${rep}.txt
  done
done
echo "Listo"