# Simulação de Controle de Tráfego Aéreo (Versões)

Projeto com duas abordagens:

1. `So.c` - Versão completa com:
   - Prioridade dinâmica e aging
   - Monitor de possível deadlock
   - Rollback de recursos
   - Starvation (alerta 60s, falha 90s)
   - Modo de teste de deadlock (`arg 5 = 1`)

2. `So_simplificado.c` - Versão reduzida (apenas headers básicos) com:
   - Prioridade simples (internacional bloqueia domésticos)
   - Ordem fixa de aquisição para evitar deadlock
   - Starvation básica (60s alerta / 90s falha)

3. `So_simple.c` - Intermediária (semáforos e lógica reduzida).

## Compilação

```
# Versão completa
gcc -pthread So.c -o simulacao

# Versão simplificada
gcc -pthread So_simplificado.c -o simples2

# Versão intermediária
gcc -pthread So_simple.c -o simples
```

## Execução

```
./simulacao <pistas> <portoes> <torres> <tempo_seg> [modo_deadlock]
./simples2  <pistas> <portoes> <torres> <tempo_seg>
./simples   <pistas> <portoes> <torres> <tempo_seg>
```

Exemplo:
```
./simulacao 3 5 2 60
./simulacao 1 1 1 40 1   # força deadlock para teste
```

## Logs
Relatórios finais são impressos no stdout. Você pode redirecionar:
```
./simulacao 3 5 2 60 > normal.log
```

## Estrutura
- `So.c` código completo com estatísticas detalhadas
- `So_simple.c` versão média
- `So_simplificado.c` versão mínima

## Licença
Uso educacional.
