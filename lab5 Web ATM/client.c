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

int main(int argc,char *argv[])
{
    if(argc!=6)
    {
        printf("input error");
        exit(0);
    }
    char *ip=argv[1];
    int port=atoi(argv[2]);
    char *action=argv[3];
    int amount=atoi(argv[4]);
    int times=atoi(argv[5]);

    int sockfd=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in serv_addr;
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family=AF_INET;
    serv_addr.sin_port=htons(port);
    inet_pton(AF_INET, ip, &serv_addr.sin_addr);
    connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr));
    char buffer[256];
    for(int i=0;i<times;i++)
    {
        snprintf(buffer,sizeof(buffer),"%s %d\n",action,amount);
        write(sockfd,buffer,strlen(buffer));
    }
    close(sockfd);
    return 0;
}
