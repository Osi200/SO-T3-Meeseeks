#!/bin/bash
# Experimento 4: D=0 con tope 60 (caos 120 s) y tope 300 (caos 30 s)
mkdir -p experimentos/resultados
printf "tiempo 0.5\ndif 0\nhola\nadios\n" | ./box 60 120 > experimentos/resultados/exp4_A.txt &
for i in 1 2 3 4 5 6; do
  echo "t=$((i*5))s procesos: $(pgrep -c box) | $(cat /proc/loadavg)"
  sleep 5
done | tee experimentos/resultados/exp4_A_sistema.txt
wait
printf "tiempo 0.5\ndif 0\nhola\nadios\n" | ./box 300 30 > experimentos/resultados/exp4_B.txt &
for i in 1 2 3 4 5 6 7 8; do
  echo "t=$((i*5))s procesos: $(pgrep -c box) | $(cat /proc/loadavg)"
  sleep 5
done | tee experimentos/resultados/exp4_B_sistema.txt
wait
echo "Listo"