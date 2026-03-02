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

int sockfd=-1;

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
}

void zombie_handler(int signum)
{
    (void)signum;
    while(waitpid(-1,NULL,WNOHANG)>0);
}

int main(int argc,char *argv[])
{
    int connfd,portno;
    socklen_t client_len;
    struct sockaddr_in serv_addr,cli_addr;
    if(argc<2)
    {
        fprintf(stderr,"ERROR, no port provided\n");
        exit(1);
    }
    signal(SIGINT,handler);
    signal(SIGCHLD,zombie_handler);

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
        pid_t childpid=fork();
        if(childpid>=0)
        {
            if(childpid==0)
            {
                close(sockfd);
                printf("The train PID is %d\n",getpid());
                fflush(stdout);
                dup2(connfd,STDOUT_FILENO);
                close(connfd);
                execlp("sl","sl","-l",NULL);
                _exit(1);
            }
            close(connfd);
        }
    }
    return 0;


}
