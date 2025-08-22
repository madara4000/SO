// Versao ainda mais simples (apenas headers basicos pedidos)
// Caracteristicas:
//  - Headers: stdio, stdlib, pthread, semaphore, time, unistd
//  - Dois tipos: Internacional (I) tem prioridade sobre Domestico (D)
//  - Fases: POUSO (pista+torre), DESEMBARQUE (portao+torre), DECOLAGEM (pista+portao+torre)
//  - Ordem de aquisicao FIXA por fase para evitar deadlock
//  - Starvation: alerta aos 60s; falha aos 90s aguardando inicio da fase atual
//  - Criacao continua de avioes ate fim do tempo de simulacao
//  - Relatorio final simples
// Limites / Simplicacoes:
//  - Sem rollback sofisticado nem aging; prioridade so bloqueia domesticos enquanto houver internacional esperando
//  - Sem temporizadores de semaforo; uso apenas de sem_wait
//  - Starvation so checado enquanto doméstico espera liberar prioridade ou entre fases
// Compilar: gcc -pthread So_simplificado.c -o simples2
// Executar: ./simples2 [pistas] [portoes] [torreSlots] [tempoSimSeg]

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>

#define MAX_AVIOES 1000
#define DUR_POUSO 3
#define DUR_DESEMBARQUE 4
#define DUR_DECOLAGEM 3
#define ALERTA_STARV 60
#define FALHA_STARV 90
#define INTERVALO_MIN_MS 300
#define INTERVALO_MAX_MS 1200

typedef enum { TIPO_INT=0, TIPO_DOM=1 } Tipo;
typedef enum { EST_CRIADO, EST_POUSO, EST_DESEMBARQUE, EST_PREP_DECOLAGEM, EST_DECOLAGEM, EST_FINAL, EST_FALHA } Estado;

typedef struct {
    int id;
    Tipo tipo;
    Estado estado;
    time_t inicio_espera; // inicio espera da proxima fase
    int alertou; // ja alertou 60s
    int falhou;  // falhou por 90s
    int ops;     // fases concluidas
    time_t ultimo_log; // para evitar spam
} Aviao;

// Parametros
int N_PISTAS=3, N_PORTOES=5, N_TORRE=2; // torre slots
int TEMPO_SIM=60; // segundos

// Recursos
sem_t sem_pista, sem_portao, sem_torre;

// Prioridade
pthread_mutex_t m_prio = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv_dom = PTHREAD_COND_INITIALIZER;
int internacionais_aguardando = 0;

// Vetores
Aviao* avioes[MAX_AVIOES];
pthread_t ths[MAX_AVIOES];
int total_avioes=0, sucessos=0, falhas=0, alertas=0;

// Tempo
time_t inicio_sim;

static time_t agora(){ return time(NULL); }
static void sleep_ms(int ms){ struct timespec ts={ ms/1000, (ms%1000)*1000000L }; nanosleep(&ts,NULL); }

static void logA(Aviao* a, const char* msg){
    printf("[%ld][A%d-%c] %s\n", (long)(agora()-inicio_sim), a->id, a->tipo==TIPO_INT?'I':'D', msg);
    fflush(stdout);
}

static void checa_starvation(Aviao* a){
    time_t t = agora();
    int espera = (int)(t - a->inicio_espera);
    if (!a->falhou && !a->alertou && espera >= ALERTA_STARV){
        a->alertou = 1; alertas++; logA(a, "ALERTA: 60s de espera");
    }
    if (!a->falhou && espera >= FALHA_STARV){
        a->falhou = 1; a->estado = EST_FALHA; falhas++; logA(a, "FALHA: 90s sem progresso");
    }
}

// Aguarda prioridade: domestico espera enquanto ha internacional contando
static void espera_prioridade(Aviao* a){
    if (a->tipo == TIPO_INT){
        pthread_mutex_lock(&m_prio); internacionais_aguardando++; pthread_mutex_unlock(&m_prio);
        return;
    }
    pthread_mutex_lock(&m_prio);
    while (internacionais_aguardando > 0){
        checa_starvation(a);
        if (a->falhou){ pthread_mutex_unlock(&m_prio); return; }
        pthread_cond_wait(&cv_dom, &m_prio);
    }
    pthread_mutex_unlock(&m_prio);
}

static void libera_prioridade(Aviao* a){
    if (a->tipo == TIPO_INT){
        pthread_mutex_lock(&m_prio);
        if (internacionais_aguardando>0) internacionais_aguardando--;
        if (internacionais_aguardando==0) pthread_cond_broadcast(&cv_dom);
        pthread_mutex_unlock(&m_prio);
    } else {
        pthread_mutex_lock(&m_prio);
        if (internacionais_aguardando==0) pthread_cond_broadcast(&cv_dom);
        pthread_mutex_unlock(&m_prio);
    }
}

// Fase Pouso: precisa pista + torre
static void fase_pouso(Aviao* a){
    a->inicio_espera = agora();
    espera_prioridade(a); if (a->falhou) return;
    sem_wait(&sem_pista); sem_wait(&sem_torre);
    libera_prioridade(a);
    a->estado = EST_POUSO; logA(a, "POUSO"); sleep(DUR_POUSO); a->ops++; a->inicio_espera = agora();
    sem_post(&sem_pista); sem_post(&sem_torre);
}

// Fase Desembarque: portao + torre (libera torre primeiro, portao apos ocupacao extra)
static void fase_desembarque(Aviao* a){
    if (a->falhou) return; a->inicio_espera=agora();
    espera_prioridade(a); if (a->falhou) return;
    sem_wait(&sem_portao); sem_wait(&sem_torre); libera_prioridade(a);
    a->estado = EST_DESEMBARQUE; logA(a, "DESEMBARQUE"); sleep(DUR_DESEMBARQUE); a->ops++; a->inicio_espera=agora();
    sem_post(&sem_torre); sleep(2); sem_post(&sem_portao);
}

// Fase Decolagem: pista + portao + torre
static void fase_decolagem(Aviao* a){
    if (a->falhou) return;
    a->estado = EST_PREP_DECOLAGEM; sleep( (rand()%3)+1 );
    a->inicio_espera = agora();
    espera_prioridade(a); if (a->falhou) return;
    sem_wait(&sem_pista); sem_wait(&sem_portao); sem_wait(&sem_torre); libera_prioridade(a);
    a->estado = EST_DECOLAGEM; logA(a, "DECOLAGEM"); sleep(DUR_DECOLAGEM); a->ops++; a->inicio_espera=agora();
    sem_post(&sem_pista); sem_post(&sem_portao); sem_post(&sem_torre);
    if (!a->falhou){ a->estado = EST_FINAL; sucessos++; }
}

static void* rotina_aviao(void* arg){
    Aviao* a = (Aviao*)arg; logA(a, "Criado"); a->inicio_espera=agora();
    fase_pouso(a); fase_desembarque(a); fase_decolagem(a);
    if (a->falhou) logA(a, "Encerrado (FALHA)"); else logA(a, "Encerrado (OK)");
    return NULL;
}

int main(int argc, char** argv){
    if (argc>1) N_PISTAS = atoi(argv[1]);
    if (argc>2) N_PORTOES = atoi(argv[2]);
    if (argc>3) N_TORRE   = atoi(argv[3]);
    if (argc>4) TEMPO_SIM = atoi(argv[4]);
    if (TEMPO_SIM<=0) TEMPO_SIM=60;

    sem_init(&sem_pista,0,N_PISTAS);
    sem_init(&sem_portao,0,N_PORTOES);
    sem_init(&sem_torre,0,N_TORRE);

    srand(time(NULL)); inicio_sim = agora();
    printf("Simplificado2: %d pistas %d portoes %d torreSlots tempo=%ds\n", N_PISTAS, N_PORTOES, N_TORRE, TEMPO_SIM);

    time_t fim_criacao = inicio_sim + TEMPO_SIM;
    int idx=0;
    while (agora() < fim_criacao && idx < MAX_AVIOES){
        Aviao* a = (Aviao*)calloc(1,sizeof(Aviao));
        a->id = idx; a->tipo = (rand()%100 < 40)? TIPO_INT : TIPO_DOM; a->estado=EST_CRIADO;
        avioes[idx]=a; pthread_create(&ths[idx], NULL, rotina_aviao, a);
        idx++; total_avioes++;
        int intervalo = INTERVALO_MIN_MS + rand()%(INTERVALO_MAX_MS-INTERVALO_MIN_MS+1);
        sleep_ms(intervalo);
    }

    for (int i=0;i<idx;i++) pthread_join(ths[i], NULL);

    printf("\n==== RELATORIO FINAL SIMPLES2 ====\n");
    printf("Criados: %d Sucesso: %d Falha: %d Alertas: %d\n", total_avioes, sucessos, falhas, alertas);
    for (int i=0;i<idx;i++){
        Aviao* a=avioes[i]; if (!a) continue;
        printf("A%d-%c estado=%d ops=%d alerta=%d falhou=%d\n", a->id, a->tipo==TIPO_INT?'I':'D', a->estado, a->ops, a->alertou, a->falhou);
        free(a);
    }
    printf("==================================\n");

    sem_destroy(&sem_pista); sem_destroy(&sem_portao); sem_destroy(&sem_torre);
    return 0;
}
