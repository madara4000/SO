// Simulação de Controle de Tráfego Aéreo com recursos limitados
// (C) Uso educacional. 
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
// Requisitos principais implementados:
// - Dois tipos de voos (INTERNACIONAL tem prioridade sobre DOMÉSTICO)
// - Recursos: Pistas (N_PISTAS), Portões (N_PORTOES) e Torre (capacidade N_TORRES, padrão 2 atendimentos simultâneos)
// - Operações: Pouso, Desembarque, Decolagem (ordem de solicitação depende do tipo de voo)
// - Prevenção/alerta de deadlock via rollback de recursos e monitor
// - Detecção de starvation (alerta crítico após 60s, falha/"queda" após 90s sem progresso); temporizador zera após cada operação concluída
// - Simulação por tempo ajustável; criação contínua de threads com intervalo randômico até término
// - Relatório final com estados, métricas, contagem de alertas
// - Código parametrizável por linha de comando: pistas portões torres tempo_simulação

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>

#define TEMPO_SIMULACAO_PADRAO 300
#define MAX_AVIOES 1000
#define INTERVALO_MONITOR 5
#define LIMITE_DEADLOCK 30
#define ALERTA_CRITICO_SECS 60
#define TEMPO_MAXIMO_ESPERA 90
#define TIMEOUT_OPERACAO 10

// Ajustes de carga (intervalos de criação de aviões)
#define INTERVALO_MIN_CRIA_MS 300
#define INTERVALO_MAX_CRIA_MS 1500

// Duracoes (simuladas) das fases em segundos (pode ajustar)
#define DUR_POUSO 3
#define DUR_DESEMBARQUE 5
#define DUR_DECOLAGEM 3
#define DUR_OCUPACAO_PORTAO_POS_DESEMBARQUE 4

int N_PISTAS = 3;
int N_PORTOES = 5;
int N_TORRES = 2; // "Uma torre" atendendo 2 aviões simultaneamente modelada como capacidade 2
int TEMPO_SIMULACAO = TEMPO_SIMULACAO_PADRAO;
int MODO_FORCAR_DEADLOCK = 0; // habilita modo de teste para provocar deadlock (sem rollback/timeouts)

// Recursos baseados em semáforos
static sem_t sem_pistas;    // contador de pistas livres
static sem_t sem_portoes;   // contador de portões livres
static sem_t sem_torre;     // contador de "slots" da torre

// Mutex para logs e estruturas compartilhadas
static pthread_mutex_t m_log = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t m_stats = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t m_wait_counts = PTHREAD_MUTEX_INITIALIZER;

// Contagem de aviões aguardando por prioridade (heurística)
static int aguardando_internacional = 0;
static int aguardando_domestico = 0;

// Enumerações de tipo e estado
typedef enum { INTERNACIONAL = 0, DOMESTICO = 1 } TipoVoo;
typedef enum {
    EST_CRIADO,
    EST_AGUARDANDO_POUSO,
    EST_POUSANDO,
    EST_AGUARDANDO_DESEMBARQUE,
    EST_DESEMBARCANDO,
    EST_AGUARDANDO_DECOLAGEM,
    EST_DECOLANDO,
    EST_FINALIZADO,
    EST_FALHA
} EstadoAviao;

typedef struct {
    int id;
    TipoVoo tipo;
    EstadoAviao estado;
    time_t inicio_espera;      // início da espera da fase corrente para starvation
    time_t ultimo_progresso;   // última vez que completou uma operação
    int alertas_starvation;    // quantos alerts (>=60s) já gerou
    int falhou;                // 1 se falhou (caiu)
    int operacoes_concluidas;  // pouso(1), desembarque(2), decolagem(3)
    int boosted;               // 1 se recebeu boost de prioridade (aging)
    int prioridade_base;       // 2 internacional, 1 doméstico
    int prioridade_efetiva;    // pode subir para 3 quando boosted
} Aviao;

static Aviao* avioes[MAX_AVIOES];
static pthread_t threads[MAX_AVIOES];
static atomic_int total_avioes = 0;
static atomic_int encerrar_criacao = 0;

// Estatísticas
static int total_sucesso = 0;
static int total_falha = 0;
static int total_alertas_starvation = 0;
static int total_alertas_deadlock = 0;
static int total_boosts = 0; // número de aviões que tiveram aging aplicado

// Tempo global de início
static time_t inicio_simulacao;

// Utilidades de tempo
static inline time_t agora() { return time(NULL); }

static void sleep_ms(int ms) {
    struct timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (long)(ms%1000)*1000000L;
    nanosleep(&ts, NULL);
}

static void log_msg(int id, TipoVoo tipo, const char* fmt, ...) {
    pthread_mutex_lock(&m_log);
    char prefixo[32];
    snprintf(prefixo, sizeof(prefixo), "[%ld][A%d-%c]", (long)(agora() - inicio_simulacao), id, (tipo==INTERNACIONAL?'I':'D'));
    printf("%s ", prefixo);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&m_log);
}

static void muda_estado(Aviao* a, EstadoAviao novo) {
    a->estado = novo;
    a->inicio_espera = agora();
}

// Função para espera com verificação de starvation e prioridade
static int aguardar_sem_timed(sem_t* sem, int segundos_timeout) {
#ifdef CLOCK_REALTIME
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += segundos_timeout;
    while (1) {
        int r = sem_timedwait(sem, &ts);
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        return -1; // timeout ou erro
    }
#else
    // Fallback simples usando try-wait + sleep (menos preciso)
    time_t inicio = time(NULL);
    while (time(NULL) - inicio < segundos_timeout) {
        if (sem_trywait(sem)==0) return 0;
        usleep(100*1000);
    }
    return -1;
#endif
}

// Checa starvation do avião; retorna 1 se caiu
static int checa_starvation(Aviao* a) {
    time_t t = agora();
    double espera = difftime(t, a->inicio_espera);
    if (!a->falhou && espera >= ALERTA_CRITICO_SECS && a->alertas_starvation==0) {
        a->alertas_starvation++;
        if (!a->boosted) {
            a->boosted = 1;
            a->prioridade_efetiva = 3; // acima de internacional padrão
            pthread_mutex_lock(&m_stats); total_boosts++; pthread_mutex_unlock(&m_stats);
            log_msg(a->id, a->tipo, "PRIORIDADE APLICADA (aging)");
        }
        pthread_mutex_lock(&m_stats);
        total_alertas_starvation++;
        pthread_mutex_unlock(&m_stats);
        log_msg(a->id, a->tipo, "ALERTA CRITICO: espera %.0fs sem progresso", espera);
    }
    if (!a->falhou && espera >= TEMPO_MAXIMO_ESPERA) {
        a->falhou = 1;
        a->estado = EST_FALHA;
        pthread_mutex_lock(&m_stats); total_falha++; pthread_mutex_unlock(&m_stats);
        log_msg(a->id, a->tipo, "FALHA: tempo máximo de espera atingido (%.0fs)", espera);
        return 1;
    }
    return 0;
}

// Aguarda um dos semáforos considerando prioridade: domestico cede se houver internacional aguardando
static int adquirir_recurso_prioritario(Aviao* a, sem_t* sem, const char* nome, int timeout_total) {
    if (MODO_FORCAR_DEADLOCK) {
        // modo simplificado bloqueante, nenhuma lógica de prioridade
        if (sem_wait(sem)==0) return 0; else return -1;
    }
    time_t inicio = agora();
    while (1) {
        // Prioridade simples determinística:
        // Ordem: doméstico boosted (3) > internacional (2) > doméstico normal (1).
        // Regra: doméstico normal só tenta se não houver internacional aguardando.
        if (a->tipo == DOMESTICO && !a->boosted) {
            int haInternacional;
            pthread_mutex_lock(&m_wait_counts);
            haInternacional = aguardando_internacional > 0;
            pthread_mutex_unlock(&m_wait_counts);
            if (haInternacional) {
                // espera um pouco e reavalia
                sleep_ms(80);
                if (checa_starvation(a)) return -1;
                if ((agora()-inicio) >= timeout_total) return -1;
                continue;
            }
        }
        // Se internacional e existe doméstico boosted aguardando, cede ao boosted
        if (a->tipo == INTERNACIONAL) {
            int existe_boost=0;
            for (int i=0;i<MAX_AVIOES;i++) { Aviao* o=avioes[i]; if (!o) continue; if (o->boosted && o->estado!=EST_FINALIZADO && o->estado!=EST_FALHA) { existe_boost=1; break; } }
            if (existe_boost) {
                sleep_ms(50);
                if (checa_starvation(a)) return -1;
                if ((agora()-inicio) >= timeout_total) return -1;
                continue;
            }
        }
        if (aguardar_sem_timed(sem, 2) == 0) return 0; // conseguiu dentro do intervalo
        if (checa_starvation(a)) return -1;
        if ((agora()-inicio) >= timeout_total) return -1;
    }
}

// Libera recursos helper
static void liberar(sem_t* sem, const char* nome) { sem_post(sem); }

// Execução de uma fase (simples sleep)
static void executa_fase(Aviao* a, const char* nome, int dur) {
    log_msg(a->id, a->tipo, "%s... (dur=%ds)", nome, dur);
    sleep(dur);
    a->ultimo_progresso = agora();
    a->inicio_espera = agora(); // zera cronômetro de starvation
    a->operacoes_concluidas++;
}

// Sequências de aquisição conforme especificado (ordem usada, mas com rollback anti-deadlock)
static int fase_pouso(Aviao* a) {
    muda_estado(a, EST_AGUARDANDO_POUSO);
    // Internacional: Pista -> Torre ; Doméstico: Torre -> Pista
    sem_t *first, *second; const char *n1, *n2;
    if (a->tipo == INTERNACIONAL) { first=&sem_pistas; second=&sem_torre; n1="PISTA"; n2="TORRE"; }
    else { first=&sem_torre; second=&sem_pistas; n1="TORRE"; n2="PISTA"; }

    int acquired_first = 0;
    if (MODO_FORCAR_DEADLOCK) {
        sem_wait(first); log_msg(a->id, a->tipo, "obteve %s para POUSO (modo deadlock)", n1);
        sleep_ms(200);
        sem_wait(second); log_msg(a->id, a->tipo, "obteve %s para POUSO (modo deadlock)", n2);
        muda_estado(a, EST_POUSANDO);
        executa_fase(a, "POUSO", DUR_POUSO);
        liberar(first, n1); liberar(second, n2);
        log_msg(a->id, a->tipo, "liberou PISTA & TORRE (POUSO concluído)");
        return 0;
    }
    while (1) {
        pthread_mutex_lock(&m_wait_counts);
        if (a->tipo==INTERNACIONAL) aguardando_internacional++; else aguardando_domestico++;
        pthread_mutex_unlock(&m_wait_counts);
        if (!acquired_first) {
            if (adquirir_recurso_prioritario(a, first, n1, TIMEOUT_OPERACAO) != 0) {
                pthread_mutex_lock(&m_wait_counts);
                if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
                pthread_mutex_unlock(&m_wait_counts);
                if (a->falhou) return -1;
                continue; // tenta novamente
            }
            acquired_first = 1;
            log_msg(a->id, a->tipo, "obteve %s para POUSO", n1);
        }
        // Tenta segundo
    if (adquirir_recurso_prioritario(a, second, n2, TIMEOUT_OPERACAO) != 0) {
            // rollback
            if (acquired_first) { liberar(first, n1); acquired_first = 0; log_msg(a->id, a->tipo, "rollback %s (POUSO)", n1); }
            pthread_mutex_lock(&m_wait_counts);
            if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
            pthread_mutex_unlock(&m_wait_counts);
            if (a->falhou) return -1;
            sleep_ms(150); // pequena espera antes de retentar
            continue;
        }
        pthread_mutex_lock(&m_wait_counts);
        if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
        pthread_mutex_unlock(&m_wait_counts);
        log_msg(a->id, a->tipo, "obteve %s para POUSO", n2);
        muda_estado(a, EST_POUSANDO);
        executa_fase(a, "POUSO", DUR_POUSO);
        liberar(first, n1); liberar(second, n2);
        log_msg(a->id, a->tipo, "liberou PISTA & TORRE (POUSO concluído)");
        return 0;
    }
}

static int fase_desembarque(Aviao* a) {
    muda_estado(a, EST_AGUARDANDO_DESEMBARQUE);
    // Internacional: Portão -> Torre ; Doméstico: Torre -> Portão
    sem_t *first, *second; const char *n1, *n2;
    if (a->tipo == INTERNACIONAL) { first=&sem_portoes; second=&sem_torre; n1="PORTAO"; n2="TORRE"; }
    else { first=&sem_torre; second=&sem_portoes; n1="TORRE"; n2="PORTAO"; }
    int acquired_first=0;
    if (MODO_FORCAR_DEADLOCK) {
        sem_wait(first); log_msg(a->id, a->tipo, "obteve %s para DESEMBARQUE (modo deadlock)", n1);
        sleep_ms(200);
        sem_wait(second); log_msg(a->id, a->tipo, "obteve %s para DESEMBARQUE (modo deadlock)", n2);
        muda_estado(a, EST_DESEMBARCANDO);
        executa_fase(a, "DESEMBARQUE", DUR_DESEMBARQUE);
        if (a->tipo == INTERNACIONAL) { liberar(second, n2); sleep(DUR_OCUPACAO_PORTAO_POS_DESEMBARQUE); liberar(first, n1); }
        else { liberar(first, n1); sleep(DUR_OCUPACAO_PORTAO_POS_DESEMBARQUE); liberar(second, n2); }
        log_msg(a->id, a->tipo, "liberou PORTAO & TORRE (DESEMBARQUE concluído)");
        return 0;
    }
    while (1) {
        pthread_mutex_lock(&m_wait_counts);
        if (a->tipo==INTERNACIONAL) aguardando_internacional++; else aguardando_domestico++;
        pthread_mutex_unlock(&m_wait_counts);
        if (!acquired_first) {
            if (adquirir_recurso_prioritario(a, first, n1, TIMEOUT_OPERACAO) != 0) {
                pthread_mutex_lock(&m_wait_counts);
                if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
                pthread_mutex_unlock(&m_wait_counts);
                if (a->falhou) return -1;
                continue;
            }
            acquired_first=1;
            log_msg(a->id, a->tipo, "obteve %s para DESEMBARQUE", n1);
        }
    if (adquirir_recurso_prioritario(a, second, n2, TIMEOUT_OPERACAO) != 0) {
            if (acquired_first) { liberar(first, n1); acquired_first=0; log_msg(a->id, a->tipo, "rollback %s (DESEMBARQUE)", n1); }
            pthread_mutex_lock(&m_wait_counts);
            if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
            pthread_mutex_unlock(&m_wait_counts);
            if (a->falhou) return -1;
            sleep_ms(150);
            continue;
        }
        pthread_mutex_lock(&m_wait_counts);
        if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
        pthread_mutex_unlock(&m_wait_counts);
        log_msg(a->id, a->tipo, "obteve %s para DESEMBARQUE", n2);
        muda_estado(a, EST_DESEMBARCANDO);
        executa_fase(a, "DESEMBARQUE", DUR_DESEMBARQUE);
        // Libera torre imediatamente, portão depois de tempo extra
        if (a->tipo == INTERNACIONAL) { liberar(second, n2); sleep(DUR_OCUPACAO_PORTAO_POS_DESEMBARQUE); liberar(first, n1); }
        else { liberar(first, n1); sleep(DUR_OCUPACAO_PORTAO_POS_DESEMBARQUE); liberar(second, n2); }
        log_msg(a->id, a->tipo, "liberou PORTAO & TORRE (DESEMBARQUE concluído)");
        return 0;
    }
}

static int fase_decolagem(Aviao* a) {
    muda_estado(a, EST_AGUARDANDO_DECOLAGEM);
    // Internacional: Portão -> Pista -> Torre ; Doméstico: Torre -> Portão -> Pista
    sem_t *r1, *r2, *r3; const char *n1,*n2,*n3;
    if (a->tipo == INTERNACIONAL) { r1=&sem_portoes; r2=&sem_pistas; r3=&sem_torre; n1="PORTAO"; n2="PISTA"; n3="TORRE"; }
    else { r1=&sem_torre; r2=&sem_portoes; r3=&sem_pistas; n1="TORRE"; n2="PORTAO"; n3="PISTA"; }
    int got1=0,got2=0;
    if (MODO_FORCAR_DEADLOCK) {
        sem_wait(r1); log_msg(a->id, a->tipo, "obteve %s para DECOLAGEM (modo deadlock)", n1);
        sleep_ms(200);
        sem_wait(r2); log_msg(a->id, a->tipo, "obteve %s para DECOLAGEM (modo deadlock)", n2);
        sleep_ms(200);
        sem_wait(r3); log_msg(a->id, a->tipo, "obteve %s para DECOLAGEM (modo deadlock)", n3);
        muda_estado(a, EST_DECOLANDO);
        executa_fase(a, "DECOLAGEM", DUR_DECOLAGEM);
        liberar(r1,n1); liberar(r2,n2); liberar(r3,n3);
        log_msg(a->id, a->tipo, "liberou todos recursos (DECOLAGEM concluída)");
        a->estado = EST_FINALIZADO;
        pthread_mutex_lock(&m_stats); total_sucesso++; pthread_mutex_unlock(&m_stats);
        return 0;
    }
    while (1) {
        pthread_mutex_lock(&m_wait_counts);
        if (a->tipo==INTERNACIONAL) aguardando_internacional++; else aguardando_domestico++;
        pthread_mutex_unlock(&m_wait_counts);
        if (!got1) {
            if (adquirir_recurso_prioritario(a, r1, n1, TIMEOUT_OPERACAO) != 0) {
                pthread_mutex_lock(&m_wait_counts);
                if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
                pthread_mutex_unlock(&m_wait_counts);
                if (a->falhou) return -1;
                continue;
            }
            got1=1; log_msg(a->id, a->tipo, "obteve %s para DECOLAGEM", n1);
        }
        if (!got2) {
            if (adquirir_recurso_prioritario(a, r2, n2, TIMEOUT_OPERACAO) != 0) {
                if (got1) { liberar(r1, n1); got1=0; log_msg(a->id, a->tipo, "rollback %s (DECOLAGEM)", n1);} 
                pthread_mutex_lock(&m_wait_counts);
                if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
                pthread_mutex_unlock(&m_wait_counts);
                if (a->falhou) return -1;
                sleep_ms(150); continue;
            }
            got2=1; log_msg(a->id, a->tipo, "obteve %s para DECOLAGEM", n2);
        }
    if (adquirir_recurso_prioritario(a, r3, n3, TIMEOUT_OPERACAO) != 0) {
            if (got2) { liberar(r2,n2); got2=0; }
            if (got1) { liberar(r1,n1); got1=0; }
            pthread_mutex_lock(&m_wait_counts);
            if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
            pthread_mutex_unlock(&m_wait_counts);
            if (a->falhou) return -1;
            sleep_ms(160); continue;
        }
        pthread_mutex_lock(&m_wait_counts);
        if (a->tipo==INTERNACIONAL) aguardando_internacional--; else aguardando_domestico--;
        pthread_mutex_unlock(&m_wait_counts);
        log_msg(a->id, a->tipo, "obteve %s para DECOLAGEM", n3);
        muda_estado(a, EST_DECOLANDO);
        executa_fase(a, "DECOLAGEM", DUR_DECOLAGEM);
        liberar(r1,n1); liberar(r2,n2); liberar(r3,n3);
        log_msg(a->id, a->tipo, "liberou todos recursos (DECOLAGEM concluída)");
        a->estado = EST_FINALIZADO;
        pthread_mutex_lock(&m_stats); total_sucesso++; pthread_mutex_unlock(&m_stats);
        return 0;
    }
}

static void* rotina_aviao(void* arg) {
    Aviao* a = (Aviao*)arg;
    log_msg(a->id, a->tipo, "Criado");
    a->inicio_espera = agora();
    a->ultimo_progresso = agora();
    a->prioridade_base = (a->tipo==INTERNACIONAL)?2:1;
    a->prioridade_efetiva = a->prioridade_base;
    if (fase_pouso(a)!=0 || a->falhou) goto fim;
    if (fase_desembarque(a)!=0 || a->falhou) goto fim;
    // Aguarda algum tempo aleatório antes de decolar (simula preparação)
    int espera = 1 + rand()%4;
    log_msg(a->id, a->tipo, "preparando para decolagem (%ds)", espera);
    sleep(espera);
    a->inicio_espera = agora();
    if (fase_decolagem(a)!=0 || a->falhou) goto fim;
fim:
    // total_falha já incrementado em checa_starvation quando falhou
    log_msg(a->id, a->tipo, "Thread encerrada estado=%d", a->estado);
    return NULL;
}

// Monitor de deadlock/starvation coletivo
static void* rotina_monitor(void* arg) {
    (void)arg;
    while (!encerrar_criacao || (atomic_load(&total_avioes) > 0)) {
        sleep(INTERVALO_MONITOR);
        int n=atomic_load(&total_avioes);
        time_t t = agora();
        int possivel_deadlock=1, ativos=0;
        for (int i=0;i<MAX_AVIOES;i++) {
            Aviao* a = avioes[i];
            if (!a) continue; if (a->estado==EST_FINALIZADO || a->estado==EST_FALHA) continue;
            ativos++;
            double semprog = difftime(t, a->ultimo_progresso);
            if (semprog < LIMITE_DEADLOCK) { possivel_deadlock=0; break; }
        }
        if (ativos>0 && possivel_deadlock) {
            pthread_mutex_lock(&m_stats); total_alertas_deadlock++; pthread_mutex_unlock(&m_stats);
            pthread_mutex_lock(&m_log);
            printf("[%.0ld][MONITOR] ALERTA: possível DEADLOCK (>%ds sem progresso coletivo)\n", (long)(t - inicio_simulacao), LIMITE_DEADLOCK);
            pthread_mutex_unlock(&m_log);
            if (MODO_FORCAR_DEADLOCK) break; // encerra monitor em modo de teste
            // Estratégia simples: liberar "gentilmente" priorizando domesticos críticos pausando priorização internacional momentaneamente
            // (Implementação leve: nada adicional aqui além do log; rollback já ajuda a quebrar ciclos.)
        }
        if (encerrar_criacao && ativos==0) break;
    }
    return NULL;
}

static void inicializar_recursos() {
    sem_init(&sem_pistas,0,N_PISTAS);
    sem_init(&sem_portoes,0,N_PORTOES);
    sem_init(&sem_torre,0,N_TORRES);
}

static void destruir_recursos() {
    sem_destroy(&sem_pistas);
    sem_destroy(&sem_portoes);
    sem_destroy(&sem_torre);
}

static void imprime_relatorio_final() {
    printf("\n===== RELATORIO FINAL =====\n");
    int total=0; for (int i=0;i<MAX_AVIOES;i++) if (avioes[i]) total++;
    printf("Total de avioes criados: %d\n", total);
    printf("Sucessos: %d | Falhas: %d\n", total_sucesso, total_falha);
    printf("Alertas starvation: %d | Alertas deadlock: %d\n", total_alertas_starvation, total_alertas_deadlock);
    printf("Boosts (aging aplicados): %d\n", total_boosts);
    printf("Estados finais por avião:\n");
    for (int i=0;i<MAX_AVIOES;i++) {
        Aviao* a = avioes[i]; if (!a) continue;
        printf("A%d-%c -> estado=%d operacoes=%d alerts=%d boosted=%d falhou=%d\n", a->id, (a->tipo==INTERNACIONAL?'I':'D'), a->estado, a->operacoes_concluidas, a->alertas_starvation, a->boosted, a->falhou);
    }
    printf("===========================\n");
}

int main(int argc, char* argv[]) {
    if (argc > 1) N_PISTAS = atoi(argv[1]);
    if (argc > 2) N_PORTOES = atoi(argv[2]);
    if (argc > 3) N_TORRES = atoi(argv[3]);
    if (argc > 4) TEMPO_SIMULACAO = atoi(argv[4]);
    if (argc > 5) MODO_FORCAR_DEADLOCK = atoi(argv[5]);
    if (TEMPO_SIMULACAO <=0) TEMPO_SIMULACAO = TEMPO_SIMULACAO_PADRAO;

    srand(time(NULL));
    inicio_simulacao = agora();
    printf("Configuração: %d pistas, %d portões, capacidade torre=%d, tempo simulação=%ds%s\n",
           N_PISTAS, N_PORTOES, N_TORRES, TEMPO_SIMULACAO,
           MODO_FORCAR_DEADLOCK?" [MODO_FORCAR_DEADLOCK]":"");
    inicializar_recursos();

    pthread_t monitor;
    pthread_create(&monitor, NULL, rotina_monitor, NULL);

    time_t fim_criacao = inicio_simulacao + TEMPO_SIMULACAO;
    int idx=0;
    while (agora() < fim_criacao && idx < MAX_AVIOES) {
        if (MODO_FORCAR_DEADLOCK && idx>=2) break; // apenas 2 aviões para caso de deadlock simples
        Aviao* a = (Aviao*)calloc(1,sizeof(Aviao));
        a->id = idx;
        if (MODO_FORCAR_DEADLOCK) {
            a->tipo = (idx==0)?INTERNACIONAL:DOMESTICO; // fixa padrão
        } else {
            a->tipo = (rand()%100 < 40) ? INTERNACIONAL : DOMESTICO; // 40% internacionais
        }
        a->estado = EST_CRIADO;
        avioes[idx] = a;
        pthread_create(&threads[idx], NULL, rotina_aviao, a);
        atomic_fetch_add(&total_avioes,1);
        idx++;
        // intervalo aleatório
        int intervalo_ms = MODO_FORCAR_DEADLOCK ? 200 : (INTERVALO_MIN_CRIA_MS + rand()%(INTERVALO_MAX_CRIA_MS-INTERVALO_MIN_CRIA_MS+1));
        sleep_ms(intervalo_ms);
    }
    encerrar_criacao = 1;
    pthread_join(monitor, NULL);
    if (MODO_FORCAR_DEADLOCK) {
        // Cancela threads possivelmente bloqueadas para finalizar e gerar relatório
        for (int i=0;i<idx;i++) {
            Aviao* a = avioes[i];
            if (a && a->estado!=EST_FINALIZADO && a->estado!=EST_FALHA) {
                pthread_cancel(threads[i]);
                a->estado = EST_FALHA;
            }
        }
    }
    for (int i=0;i<idx;i++) {
        pthread_join(threads[i], NULL);
        atomic_fetch_sub(&total_avioes,1);
    }

    imprime_relatorio_final();
    destruir_recursos();
    return 0;
}