#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/*
 Refatoração completa do simulador de controle de tráfego aéreo.
 Principais mudanças:
 - Reserva atômica de recursos com prioridade para evitar circular wait.
 - Flags por avião para recursos alocados (progresso).
 - Prioridade imediata para ALERTA_CRITICO; entre críticos, quem já possui recursos tende a avançar.
 - Liberação parcial do portão/tower no desembarque conforme especificado.
 - Contabilização de acidentes e deadlocks.
*/

#define TEMPO_SIMULACAO_PADRAO 300
#define MAX_AVIOES 1000
#define INTERVALO_MONITOR 5
#define LIMITE_DEADLOCK 30
#define ALERTA_CRITICO_SECS 60
#define TEMPO_MAXIMO_ESPERA 90
#define TIMEOUT_OPERACAO 10

/* Códigos de retorno para sem_trywait_timeout / reservar_recursos */
#define SEM_OK 0
#define SEM_TIMEOUT -1
#define SEM_SIM_ENCERRADA -2
#define SEM_AVIAO_FALHOU -3

int N_PISTAS = 3;
int N_PORTOES = 5;
int N_TORRES = 2;
int TEMPO_SIMULACAO = TEMPO_SIMULACAO_PADRAO;

sem_t pistas;
sem_t portoes;
sem_t torres;

int deadlock_count = 0;
int simulacao_ativa = 1;
int acidentes_count = 0; /* contador de quedas/incidentes */

/* Protege leituras/escritas no array de aviões */
pthread_mutex_t avioes_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Prioridade fila mutex/conds */
pthread_mutex_t fila_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_internacional = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond_domestico = PTHREAD_COND_INITIALIZER;
int esperando_internacional = 0;
int esperando_domestico = 0;

typedef enum { DOMESTICO = 0, INTERNACIONAL = 1 } TipoVoo;
typedef enum { SUCESSO = 0, FALHA = 1, ALERTA_CRITICO = 2, STARVATION = 3, DEADLOCK = 4 } StatusAviao;

typedef struct {
    int id;
    TipoVoo tipo;
    time_t criacao;
    time_t inicio_pouso, fim_pouso;
    time_t inicio_desembarque, fim_desembarque;
    time_t inicio_decolagem, fim_decolagem;
    StatusAviao status_final;
    int tempo_espera_total;
    int em_alerta_critico;
    time_t ultimo_tempo_espera;
    /* flags de recursos já adquiridos (progresso) */
    int has_pista;
    int has_portao;
    int has_torre;
} Aviao;

/* --- Reserva atômica --- */
pthread_mutex_t reserva_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t reserva_cond = PTHREAD_COND_INITIALIZER;
int pistas_disp = 0;
int portoes_disp = 0;
int torres_disp = 0;

/* --- Helpers --- */

void inicializarRecursos() {
    sem_init(&pistas, 0, N_PISTAS);
    sem_init(&portoes, 0, N_PORTOES);
    sem_init(&torres, 0, N_TORRES);

    pthread_mutex_lock(&reserva_mutex);
    pistas_disp = N_PISTAS;
    portoes_disp = N_PORTOES;
    torres_disp = N_TORRES;
    pthread_mutex_unlock(&reserva_mutex);
}

void destruirRecursos() {
    sem_destroy(&pistas);
    sem_destroy(&portoes);
    sem_destroy(&torres);
    pthread_mutex_destroy(&fila_mutex);
    pthread_cond_destroy(&cond_internacional);
    pthread_cond_destroy(&cond_domestico);
    pthread_mutex_destroy(&avioes_mutex);
    pthread_mutex_destroy(&reserva_mutex);
    pthread_cond_destroy(&reserva_cond);
}

/* marca alerta/queda por starvation / deadlock - protegido com avioes_mutex ao modificar estado */
void verificar_starvation(Aviao* aviao) {
    if (!aviao) return;

    pthread_mutex_lock(&avioes_mutex);
    if (aviao->status_final == FALHA || aviao->status_final == STARVATION || aviao->status_final == DEADLOCK) {
        pthread_mutex_unlock(&avioes_mutex);
        return;
    }

    time_t agora = time(NULL);
    time_t tempo_espera = agora - aviao->ultimo_tempo_espera;

    if (tempo_espera >= ALERTA_CRITICO_SECS && tempo_espera < TEMPO_MAXIMO_ESPERA && !aviao->em_alerta_critico) {
        printf("ALERTA CRÍTICO: Avião %d (%s) esperando há %ld segundos!\n",
               aviao->id,
               aviao->tipo == INTERNACIONAL ? "Internacional" : "Doméstico",
               (long)tempo_espera);
        aviao->em_alerta_critico = 1;
        aviao->status_final = ALERTA_CRITICO;
        /* Acorda filas e reservas para priorizar quem está em alerta */
        pthread_cond_broadcast(&cond_internacional);
        pthread_cond_broadcast(&cond_domestico);
        pthread_cond_broadcast(&reserva_cond);
    } else if (tempo_espera >= TEMPO_MAXIMO_ESPERA) {
        /* Diferenciar tratamento por tipo:
           - Domésticos -> starvation/queda (acidente)
           - Internacionais -> tratar como deadlock/falha (não contado como "acidente")
        */
        if (aviao->tipo == DOMESTICO) {
            printf("FALHA (STARVATION): Avião %d (Doméstico) caiu após %ld segundos de espera!\n",
                   aviao->id, (long)tempo_espera);
            aviao->status_final = STARVATION;
            acidentes_count++;
        } else {
            printf("FALHA (DEADLOCK): Avião %d (Internacional) sem progresso por %ld segundos — marcado DEADLOCK\n",
                   aviao->id, (long)tempo_espera);
            aviao->status_final = DEADLOCK;
            deadlock_count++;
        }
        /* Acorda filas e reservas para permitir cleanup */
        pthread_cond_broadcast(&cond_internacional);
        pthread_cond_broadcast(&cond_domestico);
        pthread_cond_broadcast(&reserva_cond);
    }
    pthread_mutex_unlock(&avioes_mutex);
}

/* checa se avião entrou em estado final de erro */
int checa_falha(Aviao* aviao) {
    verificar_starvation(aviao);
    return (aviao->status_final == FALHA || aviao->status_final == STARVATION);
}

/* sem_trywait com loop e retornos explícitos (mantido para compatibilidade) */
int sem_trywait_timeout(sem_t* sem, int segundos, Aviao* aviao) {
    time_t start = time(NULL);
    while (time(NULL) - start < segundos) {
        if (!simulacao_ativa) return SEM_SIM_ENCERRADA;
        if (checa_falha(aviao)) return SEM_AVIAO_FALHOU;
        if (sem_trywait(sem) == 0) return SEM_OK;
        usleep(100000); /* 0.1s */

        /* atualiza espera para starvation */
        pthread_mutex_lock(&avioes_mutex);
        aviao->ultimo_tempo_espera = time(NULL);
        pthread_mutex_unlock(&avioes_mutex);

        verificar_starvation(aviao);
        if (checa_falha(aviao)) return SEM_AVIAO_FALHOU;
    }
    return SEM_TIMEOUT;
}

/* Reserva atômica com prioridade e bloqueio a domésticos quando internacionais aguardam.
   need_* são necessidades REMANESCENTES (após considerar recursos já obtidos).
   Retorna SEM_OK, SEM_TIMEOUT, SEM_SIM_ENCERRADA, SEM_AVIAO_FALHOU.
*/
int reservar_recursos(Aviao* a, int need_pista, int need_portao, int need_torre, int timeout_seg) {
    time_t start = time(NULL);
    int rc = SEM_TIMEOUT;

    pthread_mutex_lock(&reserva_mutex);
    while (1) {
        if (!simulacao_ativa) { rc = SEM_SIM_ENCERRADA; break; }
        if (checa_falha(a)) { rc = SEM_AVIAO_FALHOU; break; }

        /* Prioridade imediata para alertas críticos */
        if (a->em_alerta_critico) {
            if (pistas_disp >= need_pista && portoes_disp >= need_portao && torres_disp >= need_torre) {
                pistas_disp -= need_pista;
                portoes_disp -= need_portao;
                torres_disp -= need_torre;
                rc = SEM_OK;
                break;
            }
        } else {
            /* Bloqueia domésticos se há internacionais esperando */
            if (a->tipo == DOMESTICO && esperando_internacional > 0) {
                /* não alocar, apenas aguardar */
            } else {
                if (pistas_disp >= need_pista && portoes_disp >= need_portao && torres_disp >= need_torre) {
                    pistas_disp -= need_pista;
                    portoes_disp -= need_portao;
                    torres_disp -= need_torre;
                    rc = SEM_OK;
                    break;
                }
            }
        }

        /* aguardar curto intervalo para reavaliar */
        struct timespec ts;
        time_t now = time(NULL);
        time_t rem = timeout_seg - (now - start);
        if (rem <= 0) { rc = SEM_TIMEOUT; break; }
        ts.tv_sec = now + (rem < 1 ? rem : 1);
        ts.tv_nsec = 0;
        pthread_cond_timedwait(&reserva_cond, &reserva_mutex, &ts);
    }
    pthread_mutex_unlock(&reserva_mutex);
    return rc;
}

/* libera contadores e acorda aguardantes */
void liberar_reserva(int need_pista, int need_portao, int need_torre) {
    pthread_mutex_lock(&reserva_mutex);
    pistas_disp += need_pista;
    portoes_disp += need_portao;
    torres_disp += need_torre;
    pthread_cond_broadcast(&reserva_cond);
    pthread_mutex_unlock(&reserva_mutex);
}

/* Sistema de prioridade: aviões em alerta não ficam esperando */
void aguardar_prioridade(TipoVoo tipo, Aviao* aviao) {
    pthread_mutex_lock(&fila_mutex);
    pthread_mutex_lock(&avioes_mutex);
    aviao->ultimo_tempo_espera = time(NULL);
    pthread_mutex_unlock(&avioes_mutex);

    if (tipo == INTERNACIONAL) {
        esperando_internacional++;
        while (esperando_internacional > 1 && simulacao_ativa && !checa_falha(aviao) && !aviao->em_alerta_critico) {
            struct timespec ts;
            ts.tv_sec = time(NULL) + 1;
            ts.tv_nsec = 0;
            pthread_cond_timedwait(&cond_internacional, &fila_mutex, &ts);
        }
        if (!simulacao_ativa || checa_falha(aviao)) {
            esperando_internacional--;
        }
    } else {
        esperando_domestico++;
        while (esperando_internacional > 0 && simulacao_ativa && !checa_falha(aviao) && !aviao->em_alerta_critico) {
            struct timespec ts;
            ts.tv_sec = time(NULL) + 1;
            ts.tv_nsec = 0;
            pthread_cond_timedwait(&cond_domestico, &fila_mutex, &ts);
        }
        if (!simulacao_ativa || checa_falha(aviao)) {
            esperando_domestico--;
        }
    }

    pthread_mutex_unlock(&fila_mutex);
}

/* liberar prioridade */
void liberar_prioridade(TipoVoo tipo) {
    pthread_mutex_lock(&fila_mutex);
    if (tipo == INTERNACIONAL) {
        if (esperando_internacional > 0) esperando_internacional--;
        if (esperando_internacional > 0) {
            pthread_cond_signal(&cond_internacional);
        } else if (esperando_domestico > 0) {
            pthread_cond_broadcast(&cond_domestico);
        }
    } else {
        if (esperando_domestico > 0) esperando_domestico--;
        if (esperando_domestico > 0 && esperando_internacional == 0) {
            pthread_cond_signal(&cond_domestico);
        }
    }
    pthread_mutex_unlock(&fila_mutex);
}

/* --- Operações (usando reserva atômica) --- */

/* Pouso: precisa de 1 pista + 1 torre; libera ambos após pouso. */
int pouso(Aviao* aviao) {
    if (checa_falha(aviao)) return -1;
    aguardar_prioridade(aviao->tipo, aviao);
    if (checa_falha(aviao)) { liberar_prioridade(aviao->tipo); return -1; }

    pthread_mutex_lock(&avioes_mutex);
    aviao->inicio_pouso = time(NULL);
    pthread_mutex_unlock(&avioes_mutex);

    int need_pista = aviao->has_pista ? 0 : 1;
    int need_portao = 0;
    int need_torre = aviao->has_torre ? 0 : 1;

    /* reservar remanescente */
    int r = reservar_recursos(aviao, need_pista, need_portao, need_torre, TIMEOUT_OPERACAO);
    if (r != SEM_OK) {
        if (r == SEM_TIMEOUT) {
            pthread_mutex_lock(&avioes_mutex);
            aviao->status_final = FALHA;
            pthread_mutex_unlock(&avioes_mutex);
        }
        liberar_prioridade(aviao->tipo);
        return r == SEM_OK ? 0 : (r == SEM_SIM_ENCERRADA || r == SEM_AVIAO_FALHOU ? r : -1);
    }

    /* adquira semáforos na ordem: TORRE -> PISTA (consistente) */
    if (!aviao->has_torre) {
        if (sem_wait(&torres) != 0) { liberar_reserva(need_pista, need_portao, need_torre); liberar_prioridade(aviao->tipo); return -1; }
        pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 1; pthread_mutex_unlock(&avioes_mutex);
    }
    if (!aviao->has_pista) {
        if (sem_wait(&pistas) != 0) {
            /* rollback: liberar torre se adquirida agora */
            if (aviao->has_torre) { sem_post(&torres); pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 0; pthread_mutex_unlock(&avioes_mutex); }
            liberar_reserva(need_pista, need_portao, need_torre);
            liberar_prioridade(aviao->tipo);
            return -1;
        }
        pthread_mutex_lock(&avioes_mutex); aviao->has_pista = 1; pthread_mutex_unlock(&avioes_mutex);
    }

    /* sucesso */
    printf("Avião %d pousou (%s)\n", aviao->id, aviao->tipo == INTERNACIONAL ? "Internacional" : "Doméstico");
    sleep(1);

    /* libera semáforos e reserva atômica correspondente */
    if (aviao->has_pista) { sem_post(&pistas); pthread_mutex_lock(&avioes_mutex); aviao->has_pista = 0; pthread_mutex_unlock(&avioes_mutex); }
    if (aviao->has_torre) { sem_post(&torres); pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 0; pthread_mutex_unlock(&avioes_mutex); }

    /* devolver a reserva feita (all remanescente) */
    liberar_reserva(need_pista, need_portao, need_torre);

    pthread_mutex_lock(&avioes_mutex);
    aviao->fim_pouso = time(NULL);
    aviao->ultimo_tempo_espera = aviao->fim_pouso;
    aviao->em_alerta_critico = 0;
    if (aviao->status_final == ALERTA_CRITICO) aviao->status_final = SUCESSO;
    pthread_mutex_unlock(&avioes_mutex);

    liberar_prioridade(aviao->tipo);
    return 0;
}

/* Desembarque: precisa de 1 portão + 1 torre; libera torre imediatamente e portão após tempo. */
int desembarque(Aviao* aviao) {
    if (checa_falha(aviao)) return -1;
    aguardar_prioridade(aviao->tipo, aviao);
    if (checa_falha(aviao)) { liberar_prioridade(aviao->tipo); return -1; }

    pthread_mutex_lock(&avioes_mutex);
    aviao->inicio_desembarque = time(NULL);
    pthread_mutex_unlock(&avioes_mutex);

    int need_pista = 0;
    int need_portao = aviao->has_portao ? 0 : 1;
    int need_torre = aviao->has_torre ? 0 : 1;

    int r = reservar_recursos(aviao, need_pista, need_portao, need_torre, TIMEOUT_OPERACAO);
    if (r != SEM_OK) {
        if (r == SEM_TIMEOUT) {
            pthread_mutex_lock(&avioes_mutex);
            aviao->status_final = FALHA;
            pthread_mutex_unlock(&avioes_mutex);
        }
        liberar_prioridade(aviao->tipo);
        return r == SEM_OK ? 0 : (r == SEM_SIM_ENCERRADA || r == SEM_AVIAO_FALHOU ? r : -1);
    }

    /* adquirir TORRE -> PORTÃO */
    if (!aviao->has_torre) {
        if (sem_wait(&torres) != 0) { liberar_reserva(need_pista, need_portao, need_torre); liberar_prioridade(aviao->tipo); return -1; }
        pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 1; pthread_mutex_unlock(&avioes_mutex);
    }
    if (!aviao->has_portao) {
        if (sem_wait(&portoes) != 0) {
            if (aviao->has_torre) { sem_post(&torres); pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 0; pthread_mutex_unlock(&avioes_mutex); }
            liberar_reserva(need_pista, need_portao, need_torre);
            liberar_prioridade(aviao->tipo);
            return -1;
        }
        pthread_mutex_lock(&avioes_mutex); aviao->has_portao = 1; pthread_mutex_unlock(&avioes_mutex);
    }

    /* sucesso no desembarque */
    printf("Avião %d desembarcou (%s)\n", aviao->id, aviao->tipo == INTERNACIONAL ? "Internacional" : "Doméstico");
    sleep(1);

    /* liberar torre imediatamente (tanto semáforo quanto reserva correspondente) */
    if (aviao->has_torre) { sem_post(&torres); pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 0; pthread_mutex_unlock(&avioes_mutex); }
    liberar_reserva(0, 0, need_torre); /* devolve a parte de torre da reserva agora */

    /* mantém portão ocupado por algum tempo (simula desembarque) e só depois libera portão */
    sleep(1); /* tempo que mantém o portão antes de liberar */
    if (aviao->has_portao) { sem_post(&portoes); pthread_mutex_lock(&avioes_mutex); aviao->has_portao = 0; pthread_mutex_unlock(&avioes_mutex); }
    liberar_reserva(0, need_portao, 0); /* devolve a parte de portão da reserva */

    pthread_mutex_lock(&avioes_mutex);
    aviao->fim_desembarque = time(NULL);
    aviao->ultimo_tempo_espera = aviao->fim_desembarque;
    aviao->em_alerta_critico = 0;
    if (aviao->status_final == ALERTA_CRITICO) aviao->status_final = SUCESSO;
    pthread_mutex_unlock(&avioes_mutex);

    liberar_prioridade(aviao->tipo);
    return 0;
}

/* Decolagem: precisa de portão + pista + torre; libera todos após decolagem. */
int decolagem(Aviao* aviao) {
    if (checa_falha(aviao)) return -1;
    aguardar_prioridade(aviao->tipo, aviao);
    if (checa_falha(aviao)) { liberar_prioridade(aviao->tipo); return -1; }

    pthread_mutex_lock(&avioes_mutex);
    aviao->inicio_decolagem = time(NULL);
    pthread_mutex_unlock(&avioes_mutex);

    int need_pista = aviao->has_pista ? 0 : 1;
    int need_portao = aviao->has_portao ? 0 : 1;
    int need_torre = aviao->has_torre ? 0 : 1;

    int r = reservar_recursos(aviao, need_pista, need_portao, need_torre, TIMEOUT_OPERACAO);
    if (r != SEM_OK) {
        if (r == SEM_TIMEOUT) {
            pthread_mutex_lock(&avioes_mutex);
            aviao->status_final = FALHA;
            pthread_mutex_unlock(&avioes_mutex);
        }
        liberar_prioridade(aviao->tipo);
        return r == SEM_OK ? 0 : (r == SEM_SIM_ENCERRADA || r == SEM_AVIAO_FALHOU ? r : -1);
    }

    /* adquirir recursos na ordem TORRE -> PORTÃO -> PISTA (ordem fixa) */
    if (!aviao->has_torre) {
        if (sem_wait(&torres) != 0) { liberar_reserva(need_pista, need_portao, need_torre); liberar_prioridade(aviao->tipo); return -1; }
        pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 1; pthread_mutex_unlock(&avioes_mutex);
    }
    if (!aviao->has_portao) {
        if (sem_wait(&portoes) != 0) {
            if (aviao->has_torre) { sem_post(&torres); pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 0; pthread_mutex_unlock(&avioes_mutex); }
            liberar_reserva(need_pista, need_portao, need_torre);
            liberar_prioridade(aviao->tipo);
            return -1;
        }
        pthread_mutex_lock(&avioes_mutex); aviao->has_portao = 1; pthread_mutex_unlock(&avioes_mutex);
    }
    if (!aviao->has_pista) {
        if (sem_wait(&pistas) != 0) {
            if (aviao->has_portao) { sem_post(&portoes); pthread_mutex_lock(&avioes_mutex); aviao->has_portao = 0; pthread_mutex_unlock(&avioes_mutex); }
            if (aviao->has_torre) { sem_post(&torres); pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 0; pthread_mutex_unlock(&avioes_mutex); }
            liberar_reserva(need_pista, need_portao, need_torre);
            liberar_prioridade(aviao->tipo);
            return -1;
        }
        pthread_mutex_lock(&avioes_mutex); aviao->has_pista = 1; pthread_mutex_unlock(&avioes_mutex);
    }

    /* sucesso */
    printf("Avião %d decolou (%s)\n", aviao->id, aviao->tipo == INTERNACIONAL ? "Internacional" : "Doméstico");
    sleep(1);

    /* liberar tudo */
    if (aviao->has_torre) { sem_post(&torres); pthread_mutex_lock(&avioes_mutex); aviao->has_torre = 0; pthread_mutex_unlock(&avioes_mutex); }
    if (aviao->has_portao) { sem_post(&portoes); pthread_mutex_lock(&avioes_mutex); aviao->has_portao = 0; pthread_mutex_unlock(&avioes_mutex); }
    if (aviao->has_pista) { sem_post(&pistas); pthread_mutex_lock(&avioes_mutex); aviao->has_pista = 0; pthread_mutex_unlock(&avioes_mutex); }

    /* devolver reservas correspondentes */
    liberar_reserva(need_pista, need_portao, need_torre);

    pthread_mutex_lock(&avioes_mutex);
    aviao->fim_decolagem = time(NULL);
    aviao->ultimo_tempo_espera = aviao->fim_decolagem;
    aviao->em_alerta_critico = 0;
    if (aviao->status_final == ALERTA_CRITICO) aviao->status_final = SUCESSO;
    pthread_mutex_unlock(&avioes_mutex);

    liberar_prioridade(aviao->tipo);
    return 0;
}

/* Thread principal do avião: trata retornos das operações */
void* aviao_thread(void* arg) {
    Aviao* aviao = (Aviao*)arg;

    pthread_mutex_lock(&avioes_mutex);
    aviao->ultimo_tempo_espera = time(NULL);
    aviao->has_pista = aviao->has_portao = aviao->has_torre = 0;
    pthread_mutex_unlock(&avioes_mutex);

    int r = pouso(aviao);
    if (r == SEM_SIM_ENCERRADA || r == SEM_AVIAO_FALHOU) { pthread_exit(NULL); }
    if (r != 0) { pthread_exit(NULL); }

    pthread_mutex_lock(&avioes_mutex);
    aviao->ultimo_tempo_espera = time(NULL);
    pthread_mutex_unlock(&avioes_mutex);
    r = desembarque(aviao);
    if (r == SEM_SIM_ENCERRADA || r == SEM_AVIAO_FALHOU) { pthread_exit(NULL); }
    if (r != 0) { pthread_exit(NULL); }

    pthread_mutex_lock(&avioes_mutex);
    aviao->ultimo_tempo_espera = time(NULL);
    pthread_mutex_unlock(&avioes_mutex);
    r = decolagem(aviao);
    if (r == SEM_SIM_ENCERRADA || r == SEM_AVIAO_FALHOU) { pthread_exit(NULL); }
    if (r == 0) {
        pthread_mutex_lock(&avioes_mutex);
        aviao->status_final = SUCESSO;
        pthread_mutex_unlock(&avioes_mutex);
        printf("Avião %d completou todas operações com sucesso!\n", aviao->id);
    }

    pthread_exit(NULL);
}

/* Relatório final */
void gerarRelatorioFinal(Aviao avioes[], int total) {
    int sucesso = 0, falha = 0, alerta = 0, starvation = 0, deadlocks = 0;

    printf("\n===== RELATÓRIO FINAL =====\n");
    for (int i = 0; i < total; i++) {
        Aviao* a = &avioes[i];
        if (a->id == 0) continue;

        pthread_mutex_lock(&avioes_mutex);
        StatusAviao st = a->status_final;
        pthread_mutex_unlock(&avioes_mutex);

        printf("Avião %d | Tipo: %s | Status: ",
               a->id,
               a->tipo == INTERNACIONAL ? "Internacional" : "Doméstico");

        switch (st) {
            case SUCESSO: printf("Sucesso\n"); sucesso++; break;
            case FALHA: printf("Falha\n"); falha++; break;
            case ALERTA_CRITICO: printf("Alerta Crítico\n"); alerta++; break;
            case STARVATION: printf("Starvation (queda)\n"); starvation++; break;
            case DEADLOCK: printf("Deadlock (falha)\n"); deadlocks++; break;
            default: printf("Não finalizado\n"); break;
        }
    }

    printf("\nResumo:\n");
    printf("Sucessos: %d\n", sucesso);
    printf("Falhas: %d\n", falha);
    printf("Alertas Críticos: %d\n", alerta);
    printf("Starvation (quedas): %d\n", starvation);
    printf("Deadlocks (marcados): %d\n", deadlocks);
    printf("Acidentes (quedas): %d\n", acidentes_count);
    printf("===========================\n");
}

/* Monitor de deadlock - usa mutex ao ler/modificar aviões */
void* monitor_deadlock(void* arg) {
    Aviao* avioes = (Aviao*)arg;
    while (simulacao_ativa) {
        sleep(INTERVALO_MONITOR);
        int detectados = 0;
        time_t agora = time(NULL);

        pthread_mutex_lock(&avioes_mutex);
        for (int i = 0; i < MAX_AVIOES; i++) {
            if (avioes[i].id == 0) continue;
            if (avioes[i].status_final == SUCESSO || avioes[i].status_final == FALHA || avioes[i].status_final == STARVATION || avioes[i].status_final == DEADLOCK) continue;

            if ((avioes[i].inicio_pouso > 0 && avioes[i].fim_pouso == 0 && (agora - avioes[i].inicio_pouso) > LIMITE_DEADLOCK) ||
                (avioes[i].inicio_desembarque > 0 && avioes[i].fim_desembarque == 0 && (agora - avioes[i].inicio_desembarque) > LIMITE_DEADLOCK) ||
                (avioes[i].inicio_decolagem > 0 && avioes[i].fim_decolagem == 0 && (agora - avioes[i].inicio_decolagem) > LIMITE_DEADLOCK)) {

                detectados++;
                printf("[DEADLOCK DETECTADO] Avião %d parado há mais de %ds em uma operação!\n", avioes[i].id, LIMITE_DEADLOCK);
                avioes[i].status_final = DEADLOCK;
            }
        }
        pthread_mutex_unlock(&avioes_mutex);

        deadlock_count += detectados;
    }
    pthread_exit(NULL);
}

/* --- main --- */
int main(int argc, char* argv[]) {
    if (argc > 1) N_PISTAS = atoi(argv[1]);
    if (argc > 2) N_PORTOES = atoi(argv[2]);
    if (argc > 3) N_TORRES = atoi(argv[3]);
    if (argc > 4) TEMPO_SIMULACAO = atoi(argv[4]);

    printf("Configuração: %d pistas, %d portões, %d torres, %d segundos de simulação\n",
           N_PISTAS, N_PORTOES, N_TORRES, TEMPO_SIMULACAO);

    inicializarRecursos();

    pthread_t threads[MAX_AVIOES];
    Aviao avioes[MAX_AVIOES];
    memset(avioes, 0, sizeof(avioes));
    int avioes_criados = 0;
    int id_aviao = 1;
    time_t inicio_simulacao = time(NULL);

    srand((unsigned)time(NULL));

    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, monitor_deadlock, avioes);

    while (difftime(time(NULL), inicio_simulacao) < TEMPO_SIMULACAO && avioes_criados < MAX_AVIOES) {
        Aviao* novo = &avioes[avioes_criados];
        /* Inicializa campos do avião */
        pthread_mutex_lock(&avioes_mutex);
        novo->id = id_aviao++;
        novo->tipo = (rand() % 2 == 0) ? DOMESTICO : INTERNACIONAL;
        novo->criacao = time(NULL);
        novo->status_final = SUCESSO;
        novo->em_alerta_critico = 0;
        novo->tempo_espera_total = 0;
        novo->ultimo_tempo_espera = time(NULL);
        novo->inicio_pouso = novo->fim_pouso = 0;
        novo->inicio_desembarque = novo->fim_desembarque = 0;
        novo->inicio_decolagem = novo->fim_decolagem = 0;
        novo->has_pista = novo->has_portao = novo->has_torre = 0;
        pthread_mutex_unlock(&avioes_mutex);

        if (pthread_create(&threads[avioes_criados], NULL, aviao_thread, novo) != 0) {
            perror("pthread_create");
            break;
        }
        avioes_criados++;

        usleep((500 + rand() % 1500) * 1000);
    }

    /* Parar criação de novas threads */
    printf("Tempo de simulação atingido. Novos aviões não serão criados.\n");

    /* Opcional: aguarda um pouco para permitir que threads acordem naturalmente */
    sleep(2);

    /* Encerrar a fase ativa e acordar threads bloqueadas */
    simulacao_ativa = 0;
    pthread_cond_broadcast(&cond_internacional);
    pthread_cond_broadcast(&cond_domestico);
    pthread_cond_broadcast(&reserva_cond);

    /* Postar semáforos suficientes para acordar threads bloqueadas */
    for (int i = 0; i < avioes_criados; i++) {
        sem_post(&pistas);
        sem_post(&portoes);
        sem_post(&torres);
    }

    /* Espera todas as threads dos aviões terminarem */
    for (int i = 0; i < avioes_criados; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Aguarda monitor encerrar (já vê simulacao_ativa == 0) */
    pthread_join(monitor_thread, NULL);

    /* Relatório final */
    gerarRelatorioFinal(avioes, avioes_criados);

    destruirRecursos();
    return 0;
}