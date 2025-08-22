# Simulação de Controle de Tráfego Aéreo – Versão Final
Trabalho final de SO
## Visão Geral
Esta simulação em C (POSIX threads) modela o fluxo de aviões por três fases sequenciais:
1. Pouso (pista + torre)
2. Desembarque (portão + torre, liberando a torre antes do portão)
3. Decolagem (portão + pista + torre)

### Principais características
- Prioridade para voos internacionais sobre domésticos
- Elevação imediata de prioridade para aviões em alerta crítico (starvation iminente)
- Detecção de starvation (alerta aos 60s, queda/falha aos 90s)
- Monitor periódico para detecção de deadlocks (inatividade prolongada em uma fase)
- Relatório final com estatísticas de sucesso, falhas, alertas, starvation e deadlocks

## Compilação
```
gcc -pthread So-final.c -o simulacao
```

## Execução
```
./simulacao <pistas> <portoes> <torres> <tempo_sim_seg>
```
Exemplo:
```
./simulacao 3 5 2 60
```

## Saída
Durante a execução são impressos eventos (pouso, desembarque, decolagem, alertas). Ao término é exibido o relatório consolidado.
Para salvar:
```
./simulacao 3 5 2 60 > exec.log
```

## Parâmetros Internos Relevantes
Podem ser ajustados editando o código:
- ALERTA_CRITICO_SECS (60s)
- TEMPO_MAXIMO_ESPERA (90s)
- LIMITE_DEADLOCK (30s)
- INTERVALO_MONITOR (5s)

