// Versao restrita do So.c
// Apenas headers: stdio, stdlib, pthread, semaphore, time, unistd
// Objetivo agora: aproximar comportamento da versao completa (So.c) mantendo restricao de headers.
// Funcionalidades cobertas:
//  - Dois tipos: INTERNACIONAL (prioridade base) e DOMESTICO
//  - Fases: POUSO, DESEMBARQUE, DECOLAGEM com ordens diferentes de aquisição por tipo (gera potencial deadlock)
//  - Prioridade + aging (boost domestico apos alerta de starvation)
//  - Starvation: alerta aos 60s; falha aos 90s
//  - Rollback de aquisicao parcial com timeouts por recurso (implementado via sem_trywait + polling, pois sem errno)
//  - Monitor de possivel deadlock (ausencia de progresso global > LIMITE_DEADLOCK)
//  - Modo de forcar deadlock (5o argumento = 1): desabilita rollback/timeouts, cria apenas 2 avioes (um de cada tipo) e deixa aquisicoes bloqueantes para evidenciar impasse; monitor detecta e cancela.
//  - Relatorio final com estados e metricas (sucessos, falhas, alertas, boosts, deadlock alerts)
// Nao inclui logs de cada evento para manter simplicidade textual.

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>

#define MAX_AVIOES 600
#define DUR_POUSO 3
#define DUR_DESEMBARQUE 5
#define DUR_DECOLAGEM 3
#define OCUPA_PORTAO_EXTRA 4
#define ALERTA_CRITICO 60
#define FALHA_TIMEOUT 90
#define LIMITE_DEADLOCK 30
#define INTERVALO_MONITOR 2
#define INTERVALO_MIN_MS 150
#define INTERVALO_MAX_MS 400
#define TIMEOUT_RECURSO 2       // segundos (poll cumulativo) para tentar cada recurso
#define TIMEOUT_OPERACAO 10     // segundos total tentando completar a fase antes de re-tentar do zero

typedef enum { INTERNACIONAL=0, DOMESTICO=1 } Tipo;
typedef enum { EST_CRIADO, EST_AG_POUSO, EST_POUSO, EST_AG_DESEMB, EST_DESEMB, EST_PREP_DECOL, EST_AG_DECOL, EST_DECOL, EST_FINAL, EST_FALHA } Estado;

typedef struct {
    int id;
    Tipo tipo;
    Estado estado;
    time_t inicio_espera;   // inicio de espera da fase atual
    time_t ultimo_progresso; // ultima conclusao de fase
    int alertas_starv;      // quantos alerts >=60s
    int falhou;             // 1 se falhou
    int boosted;            // 1 se aging aplicado
    int operacoes;          // fases concluidas
} Aviao;

// Parametros
int N_PISTAS=3, N_PORTOES=5, N_TORRE=2; // torres = capacidade
int TEMPO_SIM=60;
int MODO_FORCAR_DEADLOCK=0;

// Recursos
sem_t sem_pistas, sem_portoes, sem_torre;

// Sincronizacao
pthread_mutex_t m_lista = PTHREAD_MUTEX_INITIALIZER; // protege vetores e contadores
pthread_mutex_t m_prio = PTHREAD_MUTEX_INITIALIZER; // protege contagem de aguardando
pthread_mutex_t m_log = PTHREAD_MUTEX_INITIALIZER;  // logs em tempo real

// Contagem de espera para prioridade
int aguardando_internacional = 0;
int deadlock_forcado_detectado = 0;

// Vetores
Aviao* avioes[MAX_AVIOES];
pthread_t th_avioes[MAX_AVIOES];
int total_criados=0;

// Estatisticas
int total_sucesso=0;
int total_falha=0;
int total_alertas_starvation=0;
int total_alertas_deadlock=0;
int total_boosts=0;

// Progresso global
time_t ultimo_progresso_global=0;

// Controle de criacao
volatile int criando=1;

// Tempo inicio
time_t inicio_sim=0;

static time_t agora(){ return time(NULL); }
static void sleep_ms(int ms){ struct timespec ts; ts.tv_sec=ms/1000; ts.tv_nsec=(ms%1000)*1000000L; nanosleep(&ts,NULL); }
static int rand_intervalo(int a,int b){ return a + rand()%(b-a+1); }

// Starvation & aging
static void checa_starvation(Aviao* a){
    if (a->falhou || a->estado==EST_FINAL) return;
    time_t t=agora();
    int espera = (int)(t - a->inicio_espera);
    if (espera >= ALERTA_CRITICO && a->alertas_starv==0){
        a->alertas_starv=1; total_alertas_starvation++;
        if (a->tipo==DOMESTICO && !a->boosted){ a->boosted=1; total_boosts++; }
        pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] ALERTA 60s (boost=%d)\n", (long)(t-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D', a->boosted); pthread_mutex_unlock(&m_log);
    }
    if (espera >= FALHA_TIMEOUT){
        a->falhou=1; a->estado=EST_FALHA; total_falha++;
        pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] FALHA 90s\n", (long)(t-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D'); pthread_mutex_unlock(&m_log);
    }
}

// Aquisição incremental com rollback (aproxima versão completa) ou bloqueante simples se modo deadlock.
static int adquirir_fase(Aviao* a, int fase){
    if (a->falhou) return -1;
    sem_t* ordem[3]; int n=0;
    if (fase==0){ if (a->tipo==INTERNACIONAL){ ordem[n++]=&sem_pistas; ordem[n++]=&sem_torre; } else { ordem[n++]=&sem_torre; ordem[n++]=&sem_pistas; } }
    else if (fase==1){ if (a->tipo==INTERNACIONAL){ ordem[n++]=&sem_portoes; ordem[n++]=&sem_torre; } else { ordem[n++]=&sem_torre; ordem[n++]=&sem_portoes; } }
    else { if (a->tipo==INTERNACIONAL){ ordem[n++]=&sem_portoes; ordem[n++]=&sem_pistas; ordem[n++]=&sem_torre; } else { ordem[n++]=&sem_torre; ordem[n++]=&sem_portoes; ordem[n++]=&sem_pistas; } }
    time_t inicio_total = agora();
    while (!a->falhou){
        // Prioridade (cede se doméstico normal e há internacional aguardando)
        if (a->tipo==DOMESTICO && !a->boosted){
            pthread_mutex_lock(&m_prio); int ha = aguardando_internacional>0; pthread_mutex_unlock(&m_prio);
            if (ha){ sleep_ms(70); checa_starvation(a); continue; }
        }
        if (a->tipo==INTERNACIONAL){ pthread_mutex_lock(&m_prio); aguardando_internacional++; pthread_mutex_unlock(&m_prio); }
        int acquired=0;
        for (int i=0;i<n;i++){
            if (MODO_FORCAR_DEADLOCK){
                // Bloqueante direto para reproduzir impasse potencial
                sem_wait(ordem[i]);
                acquired++;
                if (i<n-1) sleep_ms(200); // aumenta janela do deadlock
                continue;
            }
            time_t inicio_recurso = agora();
            while (1){
                if (sem_trywait(ordem[i])==0){ acquired++; break; }
                sleep_ms(60);
                checa_starvation(a); if (a->falhou) break;
                if ((int)(agora()-inicio_recurso) >= TIMEOUT_RECURSO) break;
            }
            if (a->falhou) break;
            if (acquired != i+1){ // timeout nesse recurso
                // rollback dos anteriores
        for (int k=0;k<acquired;k++) sem_post(ordem[k]);
        if (acquired>0){ pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] ROLLBACK fase=%d parcial=%d\n", (long)(agora()-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D', fase, acquired); pthread_mutex_unlock(&m_log); }
        acquired=0;
                break; // sai do for para re-tentar
            }
        }
        if (a->tipo==INTERNACIONAL){ pthread_mutex_lock(&m_prio); aguardando_internacional--; pthread_mutex_unlock(&m_prio); }
        if (a->falhou) return -1;
    if (acquired==n){ pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] RECURSOS_OK fase=%d\n", (long)(agora()-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D', fase); pthread_mutex_unlock(&m_log); return 0; }
        if (MODO_FORCAR_DEADLOCK){ // em modo deadlock não faz rollback parcial (mas aqui só chega em falha se cancelado)
            return -1;
        }
        checa_starvation(a);
        if ((int)(agora()-inicio_total) >= TIMEOUT_OPERACAO){
            // recomeça contagem de operação (poderia aplicar backoff)
            inicio_total = agora();
        }
    }
    return -1;
}

static void libera_fase(Aviao* a, int fase){
    // Libera na ordem inversa de aquisição para simplicidade (tanto faz)
    if (fase==0){ // pouso
        if (a->tipo==INTERNACIONAL){ sem_post(&sem_torre); sem_post(&sem_pistas); }
        else { sem_post(&sem_pistas); sem_post(&sem_torre); }
    } else if (fase==1){ // desembarque (torre liberada antes, portao apos ocupacao extra)
        if (a->tipo==INTERNACIONAL){ sem_post(&sem_torre); sleep(OCUPA_PORTAO_EXTRA); sem_post(&sem_portoes); }
        else { sem_post(&sem_portoes); sleep(OCUPA_PORTAO_EXTRA); sem_post(&sem_torre); }
    } else { // decolagem
        if (a->tipo==INTERNACIONAL){ sem_post(&sem_torre); sem_post(&sem_pistas); sem_post(&sem_portoes); }
        else { sem_post(&sem_pistas); sem_post(&sem_portoes); sem_post(&sem_torre); }
    }
}

static void executa_fase(Aviao* a, int fase){
    if (fase==0){ a->estado=EST_POUSO; pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] POUSO_INICIO\n", (long)(agora()-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D'); pthread_mutex_unlock(&m_log); sleep(DUR_POUSO); pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] POUSO_FIM\n", (long)(agora()-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D'); pthread_mutex_unlock(&m_log); }
    else if (fase==1){ a->estado=EST_DESEMB; pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] DESEMB_INICIO\n", (long)(agora()-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D'); pthread_mutex_unlock(&m_log); sleep(DUR_DESEMBARQUE); pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] DESEMB_FIM\n", (long)(agora()-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D'); pthread_mutex_unlock(&m_log); }
    else { a->estado=EST_DECOL; pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] DECOLAGEM_INICIO\n", (long)(agora()-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D'); pthread_mutex_unlock(&m_log); sleep(DUR_DECOLAGEM); pthread_mutex_lock(&m_log); printf("[%ld][A%d-%c] DECOLAGEM_FIM\n", (long)(agora()-inicio_sim), a->id, a->tipo==INTERNACIONAL?'I':'D'); pthread_mutex_unlock(&m_log); }
    a->operacoes++; a->ultimo_progresso=agora(); ultimo_progresso_global=a->ultimo_progresso; a->inicio_espera=a->ultimo_progresso; if (fase==2) { a->estado=EST_FINAL; total_sucesso++; }
}

static void fase_pouso(Aviao* a){ a->estado=EST_AG_POUSO; a->inicio_espera=agora(); while (!a->falhou && adquirir_fase(a,0)!=0) checa_starvation(a); if (a->falhou) return; executa_fase(a,0); libera_fase(a,0); }
static void fase_desembarque(Aviao* a){ if (a->falhou) return; a->estado=EST_AG_DESEMB; a->inicio_espera=agora(); while (!a->falhou && adquirir_fase(a,1)!=0) checa_starvation(a); if (a->falhou) return; executa_fase(a,1); libera_fase(a,1); }
static void fase_decolagem(Aviao* a){ if (a->falhou) return; a->estado=EST_PREP_DECOL; sleep( rand_intervalo(1,4) ); a->estado=EST_AG_DECOL; a->inicio_espera=agora(); while (!a->falhou && adquirir_fase(a,2)!=0) checa_starvation(a); if (a->falhou) return; executa_fase(a,2); libera_fase(a,2); }

static void* rotina_aviao(void* arg){ Aviao* a=(Aviao*)arg; a->inicio_espera=agora(); a->ultimo_progresso=a->inicio_espera; fase_pouso(a); fase_desembarque(a); fase_decolagem(a); return NULL; }

static void* rotina_monitor(void* arg){ (void)arg; while (1){ sleep(INTERVALO_MONITOR); time_t t=agora(); int ativos=0; int possivel=1; pthread_mutex_lock(&m_lista); for (int i=0;i<total_criados;i++){ Aviao* a=avioes[i]; if (!a) continue; if (a->estado!=EST_FINAL && a->estado!=EST_FALHA){ ativos++; checa_starvation(a); if ((t - a->ultimo_progresso) < LIMITE_DEADLOCK) possivel=0; } } if (ativos>0 && possivel){ total_alertas_deadlock++; if (MODO_FORCAR_DEADLOCK){ deadlock_forcado_detectado=1; pthread_mutex_unlock(&m_lista); break; } } int fim = (!criando && ativos==0); pthread_mutex_unlock(&m_lista); if (fim) break; }
    return NULL; }

int main(int argc, char** argv){
    if (argc>1) N_PISTAS = atoi(argv[1]);
    if (argc>2) N_PORTOES = atoi(argv[2]);
    if (argc>3) N_TORRE   = atoi(argv[3]);
    if (argc>4) TEMPO_SIM = atoi(argv[4]);
    if (argc>5) MODO_FORCAR_DEADLOCK = atoi(argv[5]);
    if (TEMPO_SIM<=0) TEMPO_SIM=60;

    sem_init(&sem_pistas,0,N_PISTAS);
    sem_init(&sem_portoes,0,N_PORTOES);
    sem_init(&sem_torre,0,N_TORRE);

    srand(time(NULL)); inicio_sim=agora(); ultimo_progresso_global=inicio_sim;

    pthread_t monitor; pthread_create(&monitor,NULL,rotina_monitor,NULL);

    time_t fim = inicio_sim + TEMPO_SIM; int idx=0;
    while (agora()<fim && idx<MAX_AVIOES){
        if (MODO_FORCAR_DEADLOCK && idx>=2) break; // apenas dois aviões (um de cada tipo)
        Aviao* a = (Aviao*)calloc(1,sizeof(Aviao)); a->id=idx;
        if (MODO_FORCAR_DEADLOCK){ a->tipo = (idx==0)?INTERNACIONAL:DOMESTICO; }
        else { a->tipo = (rand()%100 < 40)?INTERNACIONAL:DOMESTICO; }
        a->estado=EST_CRIADO;
        pthread_mutex_lock(&m_lista); avioes[idx]=a; pthread_mutex_unlock(&m_lista);
        pthread_create(&th_avioes[idx], NULL, rotina_aviao, a); idx++; total_criados=idx;
        int intervalo = MODO_FORCAR_DEADLOCK ? 200 : (INTERVALO_MIN_MS + rand()%(INTERVALO_MAX_MS-INTERVALO_MIN_MS+1));
        sleep_ms(intervalo);
    }
    criando=0;
    pthread_join(monitor,NULL);
    if (MODO_FORCAR_DEADLOCK && deadlock_forcado_detectado){
        // Cancela threads ainda ativas para liberar e imprimir relatorio
        for (int i=0;i<total_criados;i++){ Aviao* a=avioes[i]; if (!a) continue; if (a->estado!=EST_FINAL && a->estado!=EST_FALHA){ pthread_cancel(th_avioes[i]); a->estado=EST_FALHA; a->falhou=1; if (!a->falhou) total_falha++; } }
    }
    for (int i=0;i<total_criados;i++) pthread_join(th_avioes[i],NULL);

    // Relatorio final
    printf("\n===== RELATORIO RESTRITO =====\n");
    printf("Config: pistas=%d portoes=%d torreSlots=%d tempo=%d%s\n", N_PISTAS, N_PORTOES, N_TORRE, TEMPO_SIM, MODO_FORCAR_DEADLOCK?" [MODO_DEADLOCK]":"");
    printf("Aviões criados: %d\n", total_criados);
    printf("Sucessos: %d Falhas: %d\n", total_sucesso, total_falha);
    printf("Alertas starvation: %d (boosts=%d)\n", total_alertas_starvation, total_boosts);
    printf("Alertas possivel deadlock: %d\n", total_alertas_deadlock);
    printf("Estados finais:\n");
    for (int i=0;i<total_criados;i++){
        Aviao* a=avioes[i]; if(!a) continue;
    printf("A%d-%c ef=%d fas=%d alert=%d otimi=%d falha=%d\n", a->id, a->tipo==INTERNACIONAL?'I':'D', a->estado, a->operacoes, a->alertas_starv, a->boosted, a->falhou);
        free(a);
    }
    printf("==============================\n");

    sem_destroy(&sem_pistas); sem_destroy(&sem_portoes); sem_destroy(&sem_torre);
    return 0;
}
