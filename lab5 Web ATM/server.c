#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>  
#include <arpa/inet.h>
#include<pthread.h>
#include<sys/sem.h>
#include<errno.h>
#include<sys/ipc.h>

#define  SEM_MODE 0600

int sem;
int sockfd=-1;
int balance=0;

union semun{
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

void error(const char *msg)
{
    perror(msg);
    exit(1);

}
void handler(int signum)
{
    (void)signum;
    if(sockfd>=0)
    close(sockfd);
    if (sem >= 0) {
    if (semctl(sem, 0, IPC_RMID) == -1) {
        perror("semctl IPC_RMID");
    }
}
    exit(0);

}

int V(int s)
{
    struct sembuf sop;
    sop.sem_num=0;
    sop.sem_op=1;
    sop.sem_flg=0;
    if(semop(s,&sop,1)<0)
    {
        fprintf(stderr,"V(): semop failed: %s\n",strerror(errno));
        return -1;
    }else
    {
        return 0;
    }
}

int P(int s)
{
    struct sembuf sop;
    sop.sem_num=0;
    sop.sem_op=-1;
    sop.sem_flg=0;
    if(semop(s,&sop,1)<0)
    {
        fprintf(stderr,"P(): semop failed: %s\n",strerror(errno));
        return -1;
    }else
    {
        return 0;
    }
}

void *thread_func(void *agv)
{
    int connfd=*(int *)agv;
    free(agv);
    char buffer[256];
    while(1)
    {
        ssize_t n=read(connfd,buffer,255);
        char cmd[32];
        if(n<=0) break;
        buffer[n]='\0';
        int amount;
        sscanf(buffer,"%31s %d",cmd,&amount);

        P(sem);
        if(strcmp(cmd,"deposit")==0)
        {
            balance=amount+balance;
            printf("Afer deposit: %d\n",balance);
        }else if(strcmp(cmd,"withdraw")==0)
        {
            balance=balance-amount;
            printf("Afer withdraw: %d\n",balance);
        }
        V(sem);
    }
    close(connfd);
    return NULL;
}

int main(int argc,char *argv[])
{
    int connfd,portno;
    socklen_t client_len;
    struct sockaddr_in serv_addr,cli_addr;
    long int key=0x12345;
    int val;
    sem=semget(key,1,IPC_CREAT  | SEM_MODE);
    if(sem<0)
    {
        error("semget");
    }
    union semun su;
    su.val=1;

    if(semctl(sem,0,SETVAL,su)<0)
    {
        error("error");
    }    

    if(argc<2)
    {
        fprintf(stderr,"ERROR, no port provided\n");
        exit(1);
    }
    signal(SIGINT,handler);

    portno=atoi(argv[1]);
    sockfd=socket(AF_INET,SOCK_STREAM,0);
    if(sockfd<0)
    {
        error("ERROR opening socket");
    }

    int yes=1;
    setsockopt(sockfd,SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family=AF_INET;
    serv_addr.sin_addr.s_addr=INADDR_ANY;
    serv_addr.sin_port=htons(portno);
    //bind
    if(bind(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr))<0)
    {
        error("ERROR on binding");
    }
    //listen
    listen(sockfd,5);
    while(1)
    {
        client_len=sizeof(cli_addr);
        connfd=accept(sockfd,(struct sockaddr *) &cli_addr,&client_len);
        if(connfd<0) 
        {
            break;
        }
        pthread_t thread;
        int *pconn=malloc(sizeof(int));
        *pconn=connfd;
        int rc=pthread_create(&thread,NULL,thread_func,pconn);
        if(rc!=0) 
        {
            printf("ERROR");
            close(connfd);
            exit(1);
        }
        pthread_detach(thread);
    }
    handler(0);
    return 0;


}
