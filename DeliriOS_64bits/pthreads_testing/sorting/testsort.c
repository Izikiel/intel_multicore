#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sort_code.h"
#include "tiempo.h"
#define BSP_THREAD_ID 0
#define AP_THREAD_ID 1
#define ETAPA_SORT 1
#define ETAPA_MERGE 2
#define ETAPA_COPY 3
#define ARRAY_SIZE 10
#define STATIC_ARRAY {2, 6, 8, 5, 9, 5, 2, 1, 58, 6}

typedef struct thread_data{
   long  thread_id;
   int  etapa;
   uint64_t ticksConsumed;
} THREAD_ARGS;

typedef struct measure_data{
    uint64_t sync_time_bsp;//tiempo de sincronizacion neto(solo tiempo de los join)
    uint64_t cpu_work_time_bsp;//tiempo de ejecucion neto(ejemplo sort en la etapa sort)
    uint64_t sync_time_ap;//tiempo de sincronizacion neto(solo tiempo de los join)
    uint64_t cpu_work_time_ap;//tiempo de ejecucion neto(ejemplo sort en la etapa sort)
} MEASURE_DATA;

void print_array(uint32_t array[], uint32_t array_length);
void *BSPWork(void *args);
void *APWork(void *args);
MEASURE_DATA etapaSincronizada(uint32_t etapa);
bool verfiy_sort();

uint32_t array[] = STATIC_ARRAY;
uint32_t array_length = ARRAY_SIZE;

uint32_t bsp_temp[ARRAY_SIZE];
uint32_t ap_temp[ARRAY_SIZE];

void *BSPWork(void *args)
{
    uint64_t start=0;
    uint64_t stop=0;
    THREAD_ARGS* t_args = (THREAD_ARGS*) args;
    printf("[Thread %ld] comenzando etapa %d...\n", t_args->thread_id, t_args->etapa);
        switch(t_args->etapa){
            case ETAPA_SORT:
                MEDIR_TIEMPO_START(start);
                heapsort(array, array_length/2);
                MEDIR_TIEMPO_STOP(stop);
                break;
            case ETAPA_MERGE:
                MEDIR_TIEMPO_START(start);
                limit_merge(array, bsp_temp, 0, (array_length / 2) - 1, array_length - 1, array_length / 2);
                MEDIR_TIEMPO_STOP(stop);
                break;
            case ETAPA_COPY:
                MEDIR_TIEMPO_START(start);
                copy(array, 0, bsp_temp, 0, array_length / 2);
                MEDIR_TIEMPO_STOP(stop);            
                break;
        }
    t_args->ticksConsumed = (stop - start);
    printf("[Thread %ld] etapa %d terminada.\n", t_args->thread_id, t_args->etapa); 
    pthread_exit((void*) args);
}

void *APWork(void *args)
{
    uint64_t start=0;
    uint64_t stop=0;    
    THREAD_ARGS* t_args = (THREAD_ARGS*) args;    
    printf("[Thread %ld] comenzando etapa %d...\n", t_args->thread_id, t_args->etapa);
        switch(t_args->etapa){
            case ETAPA_SORT:
                MEDIR_TIEMPO_START(start);
                heapsort((array + (array_length / 2)), (array_length / 2));
                MEDIR_TIEMPO_STOP(stop);
                break;
            case ETAPA_MERGE:  
                MEDIR_TIEMPO_START(start);
                limit_merge_reverse(array, ap_temp, 0, (array_length / 2) - 1, array_length - 1, array_length / 2);
                MEDIR_TIEMPO_STOP(stop);   
                break;
            case ETAPA_COPY:
                MEDIR_TIEMPO_START(start);
                copy(array, array_length / 2, ap_temp, 0, array_length / 2);
                MEDIR_TIEMPO_STOP(stop);
                break;
        }
    t_args->ticksConsumed = (stop - start);
    printf("[Thread %ld] etapa %d terminada.\n", t_args->thread_id, t_args->etapa);  
    pthread_exit((void*) args);
}

int main (int argc, char *argv[])
{
    MEASURE_DATA timeSort;
    MEASURE_DATA timeMerge;
    MEASURE_DATA timeCopy;
    
    memset(bsp_temp, 0, array_length * sizeof(uint32_t));
    memset(ap_temp, 0, array_length * sizeof(uint32_t));

    //imprimir array original
    printf("Arreglo original: \n");
    print_array(array, array_length);

    printf("--------------------------Comenzando etapa Sort-------------------------\n");
    timeSort = etapaSincronizada(ETAPA_SORT);
    printf("--------------------------Comenzando etapa Merge-------------------------\n");
    timeMerge = etapaSincronizada(ETAPA_MERGE);
    printf("--------------------------Comenzando etapa Copy-------------------------\n");
    timeCopy = etapaSincronizada(ETAPA_COPY);

    printf("\n--------------------------Resultados-------------------------\n");

    printf("Arreglo final: \n");
    print_array(array, array_length);
    
    if(!verfiy_sort()){
        printf("Bad sort :(\n");
            exit(-1);
    }else{
        printf("Sorting verificado! OK!\n\n");
    }
    
    printf("Tiempos Sorting:\n");
    printf("\tSync BSP: %llu", timeSort.sync_time_bsp);
    printf("\tWork BSP: %llu", timeSort.cpu_work_time_bsp);
    printf("\n\tSync AP: %llu", timeSort.sync_time_ap);
    printf("\tWork AP: %llu", timeSort.cpu_work_time_ap);
    printf("\n\n");

    printf("Tiempos Merge:\n");
    printf("\tSync BSP: %llu", timeMerge.sync_time_bsp);
    printf("\tWork BSP: %llu", timeMerge.cpu_work_time_bsp);
    printf("\n\tSync AP: %llu", timeMerge.sync_time_ap);
    printf("\tWork AP: %llu", timeMerge.cpu_work_time_ap);
    printf("\n\n");
    
    printf("Tiempos Copy:\n");
    printf("\tSync BSP: %llu", timeCopy.sync_time_bsp);
    printf("\tWork BSP: %llu", timeCopy.cpu_work_time_bsp);
    printf("\n\tSync AP: %llu", timeCopy.sync_time_ap);
    printf("\tWork AP: %llu", timeCopy.cpu_work_time_ap);
    printf("\n\n");

    printf("[Main] test completed. Exiting.\n");
    pthread_exit(NULL);
}

MEASURE_DATA etapaSincronizada(uint32_t etapaTarget){
    //mediciones de tiempo
    MEASURE_DATA timing;
    timing.sync_time_bsp = 0;
    timing.cpu_work_time_bsp = 0;    
    timing.sync_time_ap = 0;
    timing.cpu_work_time_ap = 0;    
    uint64_t start=0;
    uint64_t stop=0;

    //Crear variables para ambos threads
    pthread_t threadBsp;
    pthread_t threadAp;

    //variable de atributos para hacer los threads joineables    
    pthread_attr_t thread_attr;

    //variables auxiliares
    int returnCode;
    void* status;

    /*Seteo el atributo con PTHREAD_CREATE_JOINABLE para inicializar los threads*/
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);

    /*Creacion del BSP Thread*/
    THREAD_ARGS argsBSPSort;
    argsBSPSort.thread_id = BSP_THREAD_ID;
    argsBSPSort.etapa = etapaTarget;
    argsBSPSort.ticksConsumed = 0;
    printf("[Main] Creando Thread BSP (#%ld)\n", argsBSPSort.thread_id);
    returnCode = pthread_create(&threadBsp, &thread_attr, BSPWork, (void *) &argsBSPSort);
    if (returnCode) {
        printf("[ERROR pthread_create] return code %d\n", returnCode);
        exit(-1);
    }

    /*Creacion del AP Thread*/
    THREAD_ARGS argsAPSort;
    argsAPSort.thread_id = AP_THREAD_ID;
    argsAPSort.etapa = etapaTarget;
    argsAPSort.ticksConsumed = 0;
    printf("[Main] Creando Thread AP (#%ld)\n", argsAPSort.thread_id);
    returnCode = pthread_create(&threadAp, &thread_attr, APWork, (void *) &argsAPSort); 
                        if (returnCode) {
                            printf("[ERROR pthread_create] return code %d\n", returnCode);
                            exit(-1);
                        }

                        /* Libero los atributos*/
                        pthread_attr_destroy(&thread_attr);
    /*Espero finalizacion del BSP thread*/    
    start=0;
    stop=0;
    MEDIR_TIEMPO_START(start)
    returnCode = pthread_join(threadBsp, &status);
    MEDIR_TIEMPO_STOP(stop)
    timing.sync_time_bsp = (stop-start);
    if (returnCode) {
        printf("[ERROR pthread_join] return code %d\n", returnCode);
        exit(-1);
    }
    printf("[Main] Termino la etapa (%d) del thread BSP\n", ((THREAD_ARGS*)status)->etapa);
    timing.cpu_work_time_bsp = ((THREAD_ARGS*)status)->ticksConsumed;    
    
    /*Espero finalizacion del AP thread*/
    start=0;
    stop=0;
    MEDIR_TIEMPO_START(start)
    returnCode = pthread_join(threadAp, &status);
    MEDIR_TIEMPO_STOP(stop)
    timing.sync_time_ap = (stop-start);
    if (returnCode) {
        printf("[ERROR pthread_join] return code %d\n", returnCode);
        exit(-1);
    }
    printf("[Main] Termino la etapa (%d) del thread AP\n", ((THREAD_ARGS*)status)->etapa);
    timing.cpu_work_time_ap = ((THREAD_ARGS*)status)->ticksConsumed;
    
    return timing;
}

bool verfiy_sort()
{
    uint32_t i;
    for (i = 1; i < array_length; i++) {
        if (array[i - 1] > array[i]) {
            return false;
        }
    }
    return true;
}

void print_array(uint32_t array[], uint32_t array_length){
    int j=0;
    printf("[");
    while(j<array_length){
        if(j!=0){
            printf(", ");
        }
        printf("%d", array[j]);
        j++;
    }
    printf("]\n");
}