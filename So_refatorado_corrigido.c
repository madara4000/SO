#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/*
 Versão refatorada com correções:
 - Adicionado estado inicial PENDENTE (não marcar SUCESSO no início).
 - ALERTA_CRITICO preservado (não sobrescrito para SUCESSO ao completar fases).
 - checa_falha agora considera DEADLOCK.
 - Mantida lógica original de reserva atômica + semáforos reais.
*/

#define TEMPO_SIMULACAO_PADRAO 60
#define MAX_AVIOES 500
#define INTERVALO_MONITOR 5
#define LIMITE_DEADLOCK 30
#define ALERTA_CRITICO_SECS 60
#define TEMPO_MAXIMO_ESPERA 90
#define TIMEOUT_OPERACAO 10

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
int acidentes_count = 0;

pthread_mutex_t avioes_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t fila_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_internacional = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond_domestico = PTHREAD_COND_INITIALIZER;
int esperando_internacional = 0;
int esperando_domestico = 0;

typedef enum { DOMESTICO = 0, INTERNACIONAL = 1 } TipoVoo;
/* Adicionado PENDENTE como estado neutro inicial */
typedef enum { PENDENTE = 0, SUCESSO, FALHA, ALERTA_CRITICO, STARVATION, DEADLOCK } StatusAviao;

typedef struct {
    int id;
    TipoVoo tipo;
    time_t criacao;
    time_t inicio_pouso, fim_pouso;
    time_t inicio_desembarque, fim_desembarque;
    time_t inicio_decolagem, fim_decolagem;
    StatusAviao status_final;
    int em_alerta_critico;   /* flag ativo */
    time_t ultimo_tempo_espera;
    int has_pista, has_portao, has_torre;
} Aviao;

pthread_mutex_t reserva_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t reserva_cond = PTHREAD_COND_INITIALIZER;
int pistas_disp = 0;
int portoes_disp = 0;
int torres_disp = 0;

static inline time_t agora(){ return time(NULL); }

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
    sem_destroy(&pistas); sem_destroy(&portoes); sem_destroy(&torres);
    pthread_mutex_destroy(&fila_mutex);
    pthread_cond_destroy(&cond_internacional);
    pthread_cond_destroy(&cond_domestico);
    pthread_mutex_destroy(&avioes_mutex);
    pthread_mutex_destroy(&reserva_mutex);
    pthread_cond_destroy(&reserva_cond);
}

void verificar_starvation(Aviao* a) {
    if (!a) return;
    pthread_mutex_lock(&avioes_mutex);
    if (a->status_final == FALHA || a->status_final == STARVATION || a->status_final == DEADLOCK) {
        pthread_mutex_unlock(&avioes_mutex);
        return;
    }
    time_t t = agora();
    time_t espera = t - a->ultimo_tempo_espera;
    if (espera >= ALERTA_CRITICO_SECS && espera < TEMPO_MAXIMO_ESPERA && !a->em_alerta_critico) {
        printf("ALERTA CRITICO: Aviao %d (%s) espera %ld s\n", a->id, a->tipo==INTERNACIONAL?"Internacional":"Domestico", (long)espera);
        a->em_alerta_critico = 1;
        if (a->status_final == PENDENTE) a->status_final = ALERTA_CRITICO; /* preserva evidencia */
        pthread_cond_broadcast(&cond_internacional);
        pthread_cond_broadcast(&cond_domestico);
        pthread_cond_broadcast(&reserva_cond);
    } else if (espera >= TEMPO_MAXIMO_ESPERA) {
        if (a->tipo == DOMESTICO) {
            printf("STARVATION: Aviao %d (Domestico) caiu apos %ld s\n", a->id, (long)espera);
            a->status_final = STARVATION; acidentes_count++;
        } else {
            printf("DEADLOCK/FALHA: Aviao %d (Internacional) > %d s sem progresso\n", a->id, TEMPO_MAXIMO_ESPERA);
            a->status_final = DEADLOCK; deadlock_count++;
        }
        pthread_cond_broadcast(&cond_internacional);
        pthread_cond_broadcast(&cond_domestico);
        pthread_cond_broadcast(&reserva_cond);
    }
    pthread_mutex_unlock(&avioes_mutex);
}

int checa_falha(Aviao* a) {
    verificar_starvation(a);
    pthread_mutex_lock(&avioes_mutex);
    int f = (a->status_final == FALHA || a->status_final == STARVATION || a->status_final == DEADLOCK);
    pthread_mutex_unlock(&avioes_mutex);
    return f;
}

int reservar_recursos(Aviao* a, int need_p, int need_g, int need_t, int timeout_seg){
    time_t inicio = agora(); int rc = SEM_TIMEOUT;
    pthread_mutex_lock(&reserva_mutex);
    while (1){
        if (!simulacao_ativa){ rc = SEM_SIM_ENCERRADA; break; }
        if (checa_falha(a)){ rc = SEM_AVIAO_FALHOU; break; }
        int prioridade_alerta = a->em_alerta_critico;
        int domestico_deve_esperar = (a->tipo==DOMESTICO && esperando_internacional>0 && !prioridade_alerta);
        if (!domestico_deve_esperar){
            if (pistas_disp>=need_p && portoes_disp>=need_g && torres_disp>=need_t){
                pistas_disp-=need_p; portoes_disp-=need_g; torres_disp-=need_t; rc=SEM_OK; break;
            }
        }
        time_t agora_t = agora();
        int decorrido = (int)(agora_t - inicio);
        if (decorrido >= timeout_seg){ rc = SEM_TIMEOUT; break; }
        struct timespec ts; ts.tv_sec = agora_t + 1; ts.tv_nsec=0;
        pthread_cond_timedwait(&reserva_cond, &reserva_mutex, &ts);
    }
    pthread_mutex_unlock(&reserva_mutex);
    return rc;
}

void liberar_reserva(int p,int g,int t){
    pthread_mutex_lock(&reserva_mutex);
    pistas_disp+=p; portoes_disp+=g; torres_disp+=t;
    pthread_cond_broadcast(&reserva_cond);
    pthread_mutex_unlock(&reserva_mutex);
}

void aguardar_prioridade(TipoVoo tipo, Aviao* a){
    pthread_mutex_lock(&fila_mutex);
    if (tipo==INTERNACIONAL){
        esperando_internacional++;
        while (esperando_internacional>1 && simulacao_ativa && !checa_falha(a) && !a->em_alerta_critico){
            struct timespec ts; ts.tv_sec=agora()+1; ts.tv_nsec=0; pthread_cond_timedwait(&cond_internacional,&fila_mutex,&ts);
        }
    } else {
        esperando_domestico++;
        while (esperando_internacional>0 && simulacao_ativa && !checa_falha(a) && !a->em_alerta_critico){
            struct timespec ts; ts.tv_sec=agora()+1; ts.tv_nsec=0; pthread_cond_timedwait(&cond_domestico,&fila_mutex,&ts);
        }
    }
    pthread_mutex_unlock(&fila_mutex);
}

void liberar_prioridade(TipoVoo tipo){
    pthread_mutex_lock(&fila_mutex);
    if (tipo==INTERNACIONAL){
        if (esperando_internacional>0) esperando_internacional--;
        if (esperando_internacional>0) pthread_cond_signal(&cond_internacional);
        else if (esperando_domestico>0) pthread_cond_broadcast(&cond_domestico);
    } else {
        if (esperando_domestico>0) esperando_domestico--;
        if (esperando_domestico>0 && esperando_internacional==0) pthread_cond_signal(&cond_domestico);
    }
    pthread_mutex_unlock(&fila_mutex);
}

int pouso(Aviao* a){
    if (checa_falha(a)) return -1;
    aguardar_prioridade(a->tipo,a);
    if (checa_falha(a)){ liberar_prioridade(a->tipo); return -1; }
    a->inicio_pouso = agora(); a->ultimo_tempo_espera = a->inicio_pouso;
    int need_p = a->has_pista?0:1; int need_t = a->has_torre?0:1;
    int r = reservar_recursos(a, need_p, 0, need_t, TIMEOUT_OPERACAO);
    if (r!=SEM_OK){ if (r==SEM_TIMEOUT){ pthread_mutex_lock(&avioes_mutex); a->status_final=FALHA; pthread_mutex_unlock(&avioes_mutex);} liberar_prioridade(a->tipo); return -1; }
    if (!a->has_torre){ if (sem_wait(&torres)!=0){ liberar_reserva(need_p,0,need_t); liberar_prioridade(a->tipo); return -1;} a->has_torre=1; }
    if (!a->has_pista){ if (sem_wait(&pistas)!=0){ if (a->has_torre){ sem_post(&torres); a->has_torre=0; } liberar_reserva(need_p,0,need_t); liberar_prioridade(a->tipo); return -1;} a->has_pista=1; }
    printf("Aviao %d pouso (%s)\n", a->id, a->tipo==INTERNACIONAL?"Int":"Dom");
    sleep(1);
    if (a->has_pista){ sem_post(&pistas); a->has_pista=0; }
    if (a->has_torre){ sem_post(&torres); a->has_torre=0; }
    liberar_reserva(need_p,0,need_t);
    a->fim_pouso = agora(); a->ultimo_tempo_espera = a->fim_pouso; a->em_alerta_critico=0;
    liberar_prioridade(a->tipo);
    return 0;
}

int desembarque(Aviao* a){
    if (checa_falha(a)) return -1;
    aguardar_prioridade(a->tipo,a);
    if (checa_falha(a)){ liberar_prioridade(a->tipo); return -1; }
    a->inicio_desembarque = agora(); a->ultimo_tempo_espera = a->inicio_desembarque;
    int need_g = a->has_portao?0:1; int need_t = a->has_torre?0:1;
    int r = reservar_recursos(a,0,need_g,need_t,TIMEOUT_OPERACAO);
    if (r!=SEM_OK){ if (r==SEM_TIMEOUT){ pthread_mutex_lock(&avioes_mutex); a->status_final=FALHA; pthread_mutex_unlock(&avioes_mutex);} liberar_prioridade(a->tipo); return -1; }
    if (!a->has_torre){ if (sem_wait(&torres)!=0){ liberar_reserva(0,need_g,need_t); liberar_prioridade(a->tipo); return -1;} a->has_torre=1; }
    if (!a->has_portao){ if (sem_wait(&portoes)!=0){ if (a->has_torre){ sem_post(&torres); a->has_torre=0; } liberar_reserva(0,need_g,need_t); liberar_prioridade(a->tipo); return -1;} a->has_portao=1; }
    printf("Aviao %d desembarque (%s)\n", a->id, a->tipo==INTERNACIONAL?"Int":"Dom");
    sleep(1);
    if (a->has_torre){ sem_post(&torres); a->has_torre=0; }
    liberar_reserva(0,0,need_t); /* torre devolvida */
    sleep(1); /* ocupa portao extra */
    if (a->has_portao){ sem_post(&portoes); a->has_portao=0; }
    liberar_reserva(0,need_g,0);
    a->fim_desembarque = agora(); a->ultimo_tempo_espera = a->fim_desembarque; a->em_alerta_critico=0;
    liberar_prioridade(a->tipo);
    return 0;
}

int decolagem(Aviao* a){
    if (checa_falha(a)) return -1;
    aguardar_prioridade(a->tipo,a);
    if (checa_falha(a)){ liberar_prioridade(a->tipo); return -1; }
    a->inicio_decolagem = agora(); a->ultimo_tempo_espera = a->inicio_decolagem;
    int need_p = a->has_pista?0:1; int need_g = a->has_portao?0:1; int need_t = a->has_torre?0:1;
    int r = reservar_recursos(a,need_p,need_g,need_t,TIMEOUT_OPERACAO);
    if (r!=SEM_OK){ if (r==SEM_TIMEOUT){ pthread_mutex_lock(&avioes_mutex); a->status_final=FALHA; pthread_mutex_unlock(&avioes_mutex);} liberar_prioridade(a->tipo); return -1; }
    if (!a->has_torre){ if (sem_wait(&torres)!=0){ liberar_reserva(need_p,need_g,need_t); liberar_prioridade(a->tipo); return -1;} a->has_torre=1; }
    if (!a->has_portao){ if (sem_wait(&portoes)!=0){ if (a->has_torre){ sem_post(&torres); a->has_torre=0;} liberar_reserva(need_p,need_g,need_t); liberar_prioridade(a->tipo); return -1;} a->has_portao=1; }
    if (!a->has_pista){ if (sem_wait(&pistas)!=0){ if (a->has_portao){ sem_post(&portoes); a->has_portao=0;} if (a->has_torre){ sem_post(&torres); a->has_torre=0;} liberar_reserva(need_p,need_g,need_t); liberar_prioridade(a->tipo); return -1;} a->has_pista=1; }
    printf("Aviao %d decolagem (%s)\n", a->id, a->tipo==INTERNACIONAL?"Int":"Dom");
    sleep(1);
    if (a->has_torre){ sem_post(&torres); a->has_torre=0; }
    if (a->has_portao){ sem_post(&portoes); a->has_portao=0; }
    if (a->has_pista){ sem_post(&pistas); a->has_pista=0; }
    liberar_reserva(need_p,need_g,need_t);
    a->fim_decolagem = agora(); a->ultimo_tempo_espera = a->fim_decolagem; a->em_alerta_critico=0;
    liberar_prioridade(a->tipo);
    return 0;
}

void* aviao_thread(void* arg){
    Aviao* a=(Aviao*)arg;
    a->ultimo_tempo_espera = agora();
    if (pouso(a)==0 && !checa_falha(a)){
        if (desembarque(a)==0 && !checa_falha(a)){
            if (decolagem(a)==0 && !checa_falha(a)){
                pthread_mutex_lock(&avioes_mutex);
                if (a->status_final==PENDENTE) a->status_final=SUCESSO; /* somente se nunca alertou */
                pthread_mutex_unlock(&avioes_mutex);
                printf("Aviao %d COMPLETO (%s)%s\n", a->id, a->tipo==INTERNACIONAL?"Int":"Dom", a->status_final==ALERTA_CRITICO?" (com alerta)":"");
            }
        }
    }
    return NULL;
}

void gerarRelatorioFinal(Aviao avioes[], int total){
    int suc=0,fal=0,al=0,starv=0,dead=0,pend=0;
    printf("\n===== RELATORIO FINAL CORRIGIDO =====\n");
    for (int i=0;i<total;i++){
        Aviao* a=&avioes[i]; if (a->id==0) continue;
        StatusAviao st=a->status_final;
        switch(st){
            case SUCESSO: suc++; break;
            case FALHA: fal++; break;
            case ALERTA_CRITICO: al++; break;
            case STARVATION: starv++; break;
            case DEADLOCK: dead++; break;
            case PENDENTE: default: pend++; break;
        }
        printf("A%d-%c st=%d alerta=%d pouso=%ld desemb=%ld decol=%ld\n", a->id, a->tipo==INTERNACIONAL?'I':'D', st, a->em_alerta_critico, (long)(a->fim_pouso - a->inicio_pouso), (long)(a->fim_desembarque - a->inicio_desembarque), (long)(a->fim_decolagem - a->inicio_decolagem));
    }
    printf("Resumo:\nSucesso=%d AlertaCritico=%d Starvation=%d Deadlock=%d Falha=%d Pendente=%d\nAcidentes=%d\n", suc, al, starv, dead, fal, pend, acidentes_count);
    printf("===============================\n");
}

void* monitor_deadlock(void* arg){ Aviao* av= (Aviao*)arg; while(simulacao_ativa){ sleep(INTERVALO_MONITOR); time_t t=agora(); pthread_mutex_lock(&avioes_mutex); for (int i=0;i<MAX_AVIOES;i++){ if (av[i].id==0) continue; if (av[i].status_final==SUCESSO||av[i].status_final==FALHA||av[i].status_final==STARVATION||av[i].status_final==DEADLOCK) continue; if ((av[i].inicio_pouso>0 && av[i].fim_pouso==0 && t-av[i].inicio_pouso>LIMITE_DEADLOCK) || (av[i].inicio_desembarque>0 && av[i].fim_desembarque==0 && t-av[i].inicio_desembarque>LIMITE_DEADLOCK) || (av[i].inicio_decolagem>0 && av[i].fim_decolagem==0 && t-av[i].inicio_decolagem>LIMITE_DEADLOCK)){ printf("[MONITOR] Possivel deadlock A%d (> %ds)\n", av[i].id, LIMITE_DEADLOCK); av[i].status_final=DEADLOCK; deadlock_count++; }} pthread_mutex_unlock(&avioes_mutex);} return NULL; }

int main(int argc,char* argv[]){
    if (argc>1) N_PISTAS=atoi(argv[1]);
    if (argc>2) N_PORTOES=atoi(argv[2]);
    if (argc>3) N_TORRES=atoi(argv[3]);
    if (argc>4) TEMPO_SIMULACAO=atoi(argv[4]);
    printf("Config: %d pistas %d portoes %d torres tempo=%ds\n", N_PISTAS,N_PORTOES,N_TORRES,TEMPO_SIMULACAO);
    inicializarRecursos();
    pthread_t threads[MAX_AVIOES]; Aviao avioes[MAX_AVIOES]; memset(avioes,0,sizeof(avioes));
    int criado=0; int next_id=1; time_t inicio=agora(); srand((unsigned)time(NULL));
    pthread_t mon; pthread_create(&mon,NULL,monitor_deadlock,avioes);
    while ((int)(agora()-inicio) < TEMPO_SIMULACAO && criado < MAX_AVIOES){
        Aviao* a=&avioes[criado];
        a->id=next_id++; a->tipo=(rand()%2)?INTERNACIONAL:DOMESTICO; a->criacao=agora(); a->status_final=PENDENTE; a->em_alerta_critico=0; a->ultimo_tempo_espera=agora(); a->inicio_pouso=a->fim_pouso=0; a->inicio_desembarque=a->fim_desembarque=0; a->inicio_decolagem=a->fim_decolagem=0; a->has_pista=a->has_portao=a->has_torre=0;
        if (pthread_create(&threads[criado],NULL,aviao_thread,a)!=0){ perror("pthread_create"); break; }
        criado++;
        usleep((400 + rand()%800)*1000);
    }
    printf("Criacao encerrada. Aguardando threads...\n");
    simulacao_ativa=0; pthread_cond_broadcast(&cond_internacional); pthread_cond_broadcast(&cond_domestico); pthread_cond_broadcast(&reserva_cond);
    for (int i=0;i<criado;i++){ sem_post(&pistas); sem_post(&portoes); sem_post(&torres); }
    for (int i=0;i<criado;i++) pthread_join(threads[i],NULL);
    pthread_join(mon,NULL);
    gerarRelatorioFinal(avioes,criado);
    destruirRecursos();
    return 0;
}
