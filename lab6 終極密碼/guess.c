#include<stdlib.h>
#include<stdio.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>

#define SHMSZ 27

typedef struct {
    int guess;
    char result[8];
}data;

int shmid;
data *shm;

int done=0;
int low=1;
int high;
pid_t game_pid;
volatile  sig_atomic_t tick=0;
void timer_handler(int signum)
{
    tick=1;
}

int main(int argc,char *argv[])
{
    key_t key;
    pid_t pid=getpid();
    sscanf(argv[1],"%d",&key);
    sscanf(argv[2],"%d",&high);
    sscanf(argv[3],"%d",&game_pid);

    shmid=shmget(key,SHMSZ,IPC_CREAT|0666);
    shm=shmat(shmid,NULL,0);
    struct sigaction sa;
    memset(&sa,0,sizeof(sa));
    sa.sa_handler=&timer_handler;
    sigaction(SIGALRM,&sa,NULL);

    struct itimerval timer;
    timer.it_value.tv_sec=1;
    timer.it_value.tv_usec=0;
    timer.it_interval.tv_sec=1;
    timer.it_interval.tv_usec=0;
    setitimer(ITIMER_REAL,&timer,NULL);

    while(!done)
    {
        pause();
        if(!tick) continue;
        tick=0;
        int mid=low+(high-low)/2;
        shm->guess=mid;
        printf("[guess] Guess is %d\n",mid);
        fflush(stdout);
        shm->result[0]='\0';
        kill(game_pid,SIGUSR1);

        for(;;)
        {
            if(shm->result[0]=='\0')
            {
                usleep(1000);
                continue;
            }
            if(strcmp(shm->result,"larger")==0)
            {
                low=mid+1;
                break;
            }else if(strcmp(shm->result,"smaller")==0)
            {
                high=mid-1;
                break;
            }else if(strcmp(shm->result,"equal")==0)
            {
                done=1;
                break;
            }
            usleep(1000);
        }
    }
    shmdt(shm);
    return 0;

}
