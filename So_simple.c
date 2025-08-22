// Versao simplificada (com semaforos) da simulacao de controle de trafego aereo
// Objetivos:
//  - Código curto e direto
//  - Uso de semáforos para recursos (pistas, portões, torre)
//  - Prioridade: internacional > doméstico (doméstico espera se houver internacional aguardando)
//  - Starvation: alerta 60s, falha 90s
//  - Ordem fixa de aquisição para evitar deadlock: PISTA -> PORTAO -> TORRE (apenas para os recursos necessários naquela fase)
//  - Sem atomics / sem estruturas complexas
// Compilar: gcc -pthread So_simple.c -o simples
// Executar: ./simples [pistas] [portoes] [torreSlots] [tempoSimulacaoSeg]

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <semaphore.h>

#define MAX_AVIOES 1000
#define ALERTA_CRITICO 60
#define TIMEOUT_FALHA 90
#define DUR_POUSO 3
#define DUR_DESEMBARQUE 4
#define DUR_DECOLAGEM 3
#define INTERVALO_MIN_CRIA_MS 300
#define INTERVALO_MAX_CRIA_MS 1200

typedef enum { TIPO_INT=0, TIPO_DOM=1 } Tipo;
typedef enum { EST_CRIADO, EST_POUSO, EST_DESEMBARQUE, EST_AG_PREP_DECOLAGEM, EST_DECOLAGEM, EST_FINAL, EST_FALHA } Estado;

typedef struct {
    int id;
    Tipo tipo;
    Estado estado;
    time_t inicio_espera; // inicio espera da fase
    time_t ultimo_prog;   // ultima conclusao
    int alertou;
    int falhou;
    int ops; // fases concluidas
} Aviao;

// Parametros / estado global (sem static conforme pedido)
int pistas=3, portoes=5, torres=2;
int sim_tempo=60;
volatile int criando=1;
time_t inicio_sim;

// Semáforos de recursos
sem_t sem_pista;
sem_t sem_portao;
sem_t sem_torre;

// Sincronização para prioridade
pthread_mutex_t m_prior = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv_prio = PTHREAD_COND_INITIALIZER;
int aguardando_int=0; // número de internacionais aguardando aquisição

// Vetores
Aviao* avioes[MAX_AVIOES];
pthread_t ths[MAX_AVIOES];
int total=0, sucesso=0, falha=0, alertas=0;

static time_t agora(){ return time(NULL); }
static void sleep_ms(int ms){ struct timespec ts={ms/1000,(ms%1000)*1000000L}; nanosleep(&ts,NULL); }

static void logA(Aviao* a, const char* msg){
    printf("[%ld][A%d-%c] %s\n", (long)(agora()-inicio_sim), a->id, a->tipo==TIPO_INT?'I':'D', msg);
    fflush(stdout);
}

void checa_starvation(Aviao* a){
    double espera = difftime(agora(), a->inicio_espera);
    if (!a->falhou && !a->alertou && espera >= ALERTA_CRITICO){
        a->alertou=1; alertas++; logA(a, "ALERTA 60s sem progresso");
    }
    if (!a->falhou && espera >= TIMEOUT_FALHA){
        a->falhou=1; a->estado=EST_FALHA; falha++; logA(a, "FALHA 90s sem progresso");
    }
}

// Adquire recursos necessários em ordem única: pista -> portão -> torre
// Para cada recurso não requerido (need_*==0) simplesmente ignora.
int adquirir(Aviao* a, int need_p, int need_port, int need_torre){
    // Sinaliza que internacional vai tentar (para bloquear domésticos)
    if (a->tipo==TIPO_INT){
        pthread_mutex_lock(&m_prior); aguardando_int++; pthread_mutex_unlock(&m_prior);
    } else {
        // doméstico aguarda enquanto existe internacional aguardando
        pthread_mutex_lock(&m_prior);
        while (aguardando_int>0){
            checa_starvation(a); if (a->falhou){ pthread_mutex_unlock(&m_prior); return -1; }
            pthread_cond_wait(&cv_prio,&m_prior);
        }
        pthread_mutex_unlock(&m_prior);
    }
    // Aquisição sequencial (evita deadlock porque todos seguem mesma ordem)
    for (int i=0;i<need_p;i++)   { if (sem_wait(&sem_pista)!=0) return -1; }
    for (int i=0;i<need_port;i++){ if (sem_wait(&sem_portao)!=0) return -1; }
    for (int i=0;i<need_torre;i++){ if (sem_wait(&sem_torre)!=0) return -1; }
    // Internacional deixa de contar
    if (a->tipo==TIPO_INT){
        pthread_mutex_lock(&m_prior); aguardando_int--; if (aguardando_int==0) pthread_cond_broadcast(&cv_prio); pthread_mutex_unlock(&m_prior);
    }
    return 0;
}

void liberar(int p, int port, int t){
    for (int i=0;i<p;i++) sem_post(&sem_pista);
    for (int i=0;i<port;i++) sem_post(&sem_portao);
    for (int i=0;i<t;i++) sem_post(&sem_torre);
    // Se não há internacionais aguardando, acorda domésticos (broadcast)
    pthread_mutex_lock(&m_prior); if (aguardando_int==0) pthread_cond_broadcast(&cv_prio); pthread_mutex_unlock(&m_prior);
}

void conclui_fase(Aviao* a){ a->ultimo_prog=agora(); a->inicio_espera=agora(); a->ops++; }

void fase_pouso(Aviao* a){
    a->inicio_espera=agora();
    if (adquirir(a,1,0,1)!=0) return; // pista + torre
    a->estado=EST_POUSO; logA(a, "POUSO"); sleep(DUR_POUSO); conclui_fase(a); liberar(1,0,1);
}
void fase_desembarque(Aviao* a){
    if (a->falhou) return; a->inicio_espera=agora();
    if (adquirir(a,0,1,1)!=0) return; a->estado=EST_DESEMBARQUE; logA(a,"DESEMBARQUE"); sleep(DUR_DESEMBARQUE); conclui_fase(a); // libera torre logo, portao depois de pequena ocupacao
    liberar(0,0,1); sleep(2); liberar(0,1,0);
}
void fase_decolagem(Aviao* a){
    if (a->falhou) return; a->inicio_espera=agora(); a->estado=EST_AG_PREP_DECOLAGEM; sleep( (rand()%3)+1 ); // prep
    a->inicio_espera=agora(); if (adquirir(a,1,1,1)!=0) return; a->estado=EST_DECOLAGEM; logA(a,"DECOLAGEM"); sleep(DUR_DECOLAGEM); conclui_fase(a); liberar(1,1,1); a->estado=EST_FINAL; sucesso++;
}

void* rotina(void* arg){
    Aviao* a=(Aviao*)arg; logA(a,"Criado"); a->inicio_espera=agora(); a->ultimo_prog=agora();
    fase_pouso(a); fase_desembarque(a); fase_decolagem(a);
    if (a->falhou) logA(a,"Encerrado (FALHA)"); else logA(a,"Encerrado (OK)");
    return NULL;
}

int main(int argc, char** argv){
    if (argc>1) pistas=atoi(argv[1]);
    if (argc>2) portoes=atoi(argv[2]);
    if (argc>3) torres=atoi(argv[3]);
    if (argc>4) sim_tempo=atoi(argv[4]);
    sem_init(&sem_pista,0,pistas);
    sem_init(&sem_portao,0,portoes);
    sem_init(&sem_torre,0,torres);
    srand(time(NULL)); inicio_sim=agora();
    printf("Simples: %d pistas %d portoes %d torreSlots tempo=%ds\n", pistas, portoes, torres, sim_tempo);
    time_t fim = inicio_sim + sim_tempo;
    int idx=0;
    while (agora()<fim && idx<MAX_AVIOES){
        Aviao* a = calloc(1,sizeof(Aviao)); a->id=idx; a->tipo = (rand()%100<40)?TIPO_INT:TIPO_DOM; avioes[idx]=a;
        pthread_create(&ths[idx],NULL,rotina,a); idx++; total++;
        int intervalo = INTERVALO_MIN_CRIA_MS + rand()%(INTERVALO_MAX_CRIA_MS-INTERVALO_MIN_CRIA_MS+1);
        sleep_ms(intervalo);
    }
    criando=0;
    for (int i=0;i<idx;i++) pthread_join(ths[i],NULL);

    printf("\n==== RELATORIO SIMPLES ====\n");
    printf("Criados: %d Sucesso: %d Falha: %d Alertas: %d\n", total, sucesso, falha, alertas);
    for (int i=0;i<idx;i++){
        Aviao* a=avioes[i]; if (!a) continue;
        printf("A%d-%c estado=%d ops=%d alerta=%d falhou=%d\n", a->id, a->tipo==TIPO_INT?'I':'D', a->estado, a->ops, a->alertou, a->falhou);
        free(a);
    }
    printf("===========================\n");
    sem_destroy(&sem_pista); sem_destroy(&sem_portao); sem_destroy(&sem_torre);
    return 0;
}
