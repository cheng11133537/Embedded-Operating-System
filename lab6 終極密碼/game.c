#include<stdio.h>
#include<stdlib.h>
#include<sys/ipc.h>
#include<sys/types.h>
#include<unistd.h>
#include<signal.h>      
#include<string.h>      
#include<sys/shm.h>     


#define SHMSZ 27

typedef struct {
    int guess;
    char result[8];
}data;

int shmid;
data *shm;
int guess;

void CtrlCHandler(int signum)
{
    if(signum==SIGINT)
    {
        shmctl(shmid,IPC_RMID,NULL);
        exit(1);
    }
}

void SIGUSR1_handler(int signo, siginfo_t *info, void *context)
{

    if(shm->guess<guess)
    {
        printf("Larger\n");
        strncpy(shm->result,"larger",sizeof(shm->result));
        shm->result[sizeof(shm->result)-1]='\0';
    }else if(shm->guess>guess)
    {
        printf("Smaller\n");
        strncpy(shm->result,"smaller",sizeof(shm->result));
        shm->result[sizeof(shm->result)-1]='\0';
    }else{
        printf("Bingo\n");
        strncpy(shm->result,"equal",sizeof(shm->result));
        shm->result[sizeof(shm->result)-1]='\0';
    }
    fflush(stdout);
}
int main(int argc,char *argv[])
{
    pid_t pid=getpid();
    printf("[game]Game pid is %d\n",pid);
    key_t key;
    sscanf(argv[1],"%d",&key);
    sscanf(argv[2],"%d",&guess);
    shmid=shmget(key,SHMSZ,IPC_CREAT|0666);
    shm=shmat(shmid,NULL,0);

    struct sigaction SIGUSR1_sa;
    memset(&SIGUSR1_sa, 0, sizeof (SIGUSR1_sa));
    SIGUSR1_sa.sa_flags = SA_SIGINFO;
    SIGUSR1_sa.sa_sigaction = SIGUSR1_handler;
    sigaction (SIGUSR1, &SIGUSR1_sa, NULL);

    struct sigaction CtrlC_sa;
    memset(&CtrlC_sa, 0, sizeof (CtrlC_sa));
    CtrlC_sa.sa_handler = CtrlCHandler;
    sigaction (SIGINT, &CtrlC_sa, NULL);

    while (1);
    return 0;

}
