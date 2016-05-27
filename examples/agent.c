#include <stdio.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h> 
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

/* Create a client endpoint and connect to a server.   Returns fd if all OK, <0 on error. */
int
unix_socket_conn(const char *servername){
    int fd; 
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {   /* create a UNIX domain stream socket */
        return(-1);
    }
    int len, rval;
    struct sockaddr_un un;          
    memset(&un, 0, sizeof(un));            /* fill socket address structure with our address */
    un.sun_family = AF_UNIX; 
    sprintf(un.sun_path, "scktmp%05d", getpid()); 
    len = offsetof(struct sockaddr_un, sun_path) + strlen(un.sun_path);
    unlink(un.sun_path);               /* in case it already exists */ 
    if (bind(fd, (struct sockaddr *)&un, len) < 0){ 
    	  rval=  -2; 
    } 
    else{
  	/* fill socket address structure with server's address */
    	  memset(&un, 0, sizeof(un)); 
    	  un.sun_family = AF_UNIX; 
    	  strcpy(un.sun_path, servername); 
    	  len = offsetof(struct sockaddr_un, sun_path) + strlen(servername); 
    	  if (connect(fd, (struct sockaddr *)&un, len) < 0) {
      		  rval= -4; 
    	  } 
    	  else {
  	        return (fd);
    	  }
    }
    int err;
    err = errno;
    close(fd); 
    errno = err;
    return rval;
}
 
void
unix_socket_close(int fd){
    close(fd);     
}

int
main(void){
    srand((int)time(0));
    int connfd; 
    connfd = unix_socket_conn("pbs.buffer1");
    if(connfd<0){
        printf("Error[%d] when connecting...",errno);
    	  return 0;
    }
    printf("Begin to recv/send...\n");
    int size;
    int sndbuf[2];
    for(;;){
        //=========sending======================
        printf("Enter the interface and mode here: ");
        if(scanf("%d %d",&sndbuf[0], &sndbuf[1])<0){
            printf("Scanf error");
            return 0;
        }
        getchar();
        size = send(connfd, (char*)sndbuf, 8, 0);
      	if(size>=0){
    		    printf("Data[%d] Sended:%c.\n",size,sndbuf[0]);
      	}
    	  if(size==-1){
    	      printf("Error[%d] when Sending Data:%s.\n",errno,strerror(errno));	 
            break;		
    	  }
        sleep(1);
    }
    unix_socket_close(connfd);
    printf("Agent exited.\n");
    return 0;
}