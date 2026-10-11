#!/bin/bash
# Experimento 2: tiempo fijo vs aleatorio, con dificultad 40
mkdir -p experimentos/resultados
for modo in "fijo 1" "aleatorio"; do
  nombre=$(echo $modo | tr ' ' '_')
  for rep in 1 2 3 4 5; do
    printf "tiempo $modo\ndif 40\nhola\nadios\n" | ./box 200 120 \
      > experimentos/resultados/exp2_${nombre}_r${rep}.txt
  done
done
echo "Listo"