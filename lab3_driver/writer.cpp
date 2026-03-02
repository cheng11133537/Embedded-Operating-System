#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

int main(int argc,char *argv[])
{
    
    if(argc!=2)              
    {
        fprintf(stderr,"Usage:./writer <studentID>\n"); 
        return 1;
    }
    const char *studentID=argv[1];
    int len=strlen(studentID);
    int fd=open("/dev/etx_device", O_WRONLY); 
    if (fd < 0) {
        perror("open");  
        return 1;
    }
    printf("writer: sending \"%s\"\n", studentID);
    for(size_t i=0;i<len;i++)
    {
        char c=studentID[i];
        if(write(fd, &c, 1) != 1) 
        {
            perror("write");
            close(fd);
            return 1;
        }
        sleep(1);
        
    }
    close(fd);
    return 0;
}
