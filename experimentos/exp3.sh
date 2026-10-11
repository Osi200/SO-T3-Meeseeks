#!/bin/bash
# Experimento 3: costo de los tres tipos de solicitud
mkdir -p experimentos/resultados
for rep in 1 2 3 4 5; do
  printf "tiempo 0.2\ndif 90\nhola\ncalc (4+3*2)/5\nexec ls /tmp\nexec false\ncalc 10/0\nadios\n" \
    | ./box 200 60 > experimentos/resultados/exp3_r${rep}.txt
done
echo "Listo"