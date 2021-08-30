/*
** 2021-08-30
**
** Author: Li QingLin  
**
*************************************************************************
**
** This source code file implements a small, simple websocket server. 
**
** It takes care of handling the WebSocket connections, launching your programs to handle the WebSockets,
** and passing messages between programs and web-browser.
**
** Just read incoming text from stdin and write outgoing text to stdout. 
**
**   .............                                  ...............................
**   .           .                                  .         --stdout--> program .                    
**   . web-brower. <------internet----------------->. wsdapper                    .
**   .           .                                  .         <--stdin--  program .                    
**   .............                                  ...............................
**    client side                                       websocket server side
**
**
**
** Features:
**
**     * One process per request(client)
**     * Programs runs in a chroot jail
**     * Simple setup - no configuration files to misconfigure
**     * A single C-code file with no dependences outside of the standard C library.
** 
** Setup rules:
**
**    (1) To build and install wsdapper, run the following command: 
**
**         gcc -Os -o wsdapper wsdapper.c get the wsdapper 
**
**    (2) run it:
**
**         $ wsdapper --port=8080 my-program
**
** Command-line Options:
**
**
**  --port N         Listening on TCP port N
**
**
** Security Features:
**
** (1)  This program automatically puts itself inside a chroot jail if
**      it can and if not specifically prohibited by the "--jail 0"
**      command-line option.  The root of the jail is the directory that
**      contains the various $HOST.website content subdirectories.
**
**
*/
#include <stdio.h>
#include <ctype.h>
#include <syslog.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <time.h>
#include <sys/times.h>
#include <netdb.h>
#include <errno.h>
#include <sys/resource.h>
#include <signal.h>
#ifdef linux
#include <sys/sendfile.h>
#endif
#include <assert.h>

/*
** Configure the server by setting the following macros and recompiling.
*/
#ifndef DEFAULT_PORT
#define DEFAULT_PORT "8001"             /* Default TCP port for Websocket */
#endif
#ifndef MAX_CONTENT_LENGTH
#define MAX_CONTENT_LENGTH 250000000  /* Max length of data request content */
#endif
#ifndef MAX_CPU
#define MAX_CPU 30                /* Max CPU cycles in seconds */
#endif
#ifndef BACKLOG
#define BACKLOG 20		/*listen(fd, backlog) */
#endif



/*
** We record most of the state information as global variables.  This
** saves having to pass information to subroutines as parameters, and
** makes the executable smaller...
*/
static char *sRoot = 0;          /* Root directory of the website */
static int ipv6Only = 0;         /* Use IPv6 only */
static int ipv4Only = 0;         /* Use IPv4 only */



#define MAX_PARALLEL 50  /* Number of simultaneous children */

/*
** All possible forms of an IP address.  Needed to work around GCC strict
** aliasing rules.
*/
typedef union {
  struct sockaddr sa;              /* Abstract superclass */
  struct sockaddr_in sa4;          /* IPv4 */
  struct sockaddr_in6 sa6;         /* IPv6 */
  struct sockaddr_storage sas;     /* Should be the maximum of the above 3 */
} address;

/*
** Implement an ws server daemon listening on port sPort.
**
** As new connections arrive, fork a child and let the child return
** out of this procedure call.  The child will handle the request.
** The parent never returns from this procedure.
**
** Return 0 to each child as it runs.  If unable to establish a
** listening socket, return non-zero.
*/
int ws_server(const char *sPort, int localOnly){
	int listener[20];            /* The server sockets */
	int connection;              /* A socket for each individual connection */
	fd_set readfds;              /* Set of file descriptors for select() */
	address inaddr;              /* Remote address */
	socklen_t lenaddr;           /* Length of the inaddr structure */
	int child;                   /* PID of the child process */
	int nchildren = 0;           /* Number of child processes */
	struct timeval delay;        /* How long to wait inside select() */
	int opt = 1;                 /* setsockopt flag */
	struct addrinfo sHints;      /* Address hints */
	struct addrinfo *pAddrs, *p; /* */
	int rc;                      /* Result code */
	int i, n;
	int maxFd = -1;	

	memset(&sHints, 0, sizeof(sHints));
	if( ipv4Only ){
		sHints.ai_family = PF_INET;
		/*printf("ipv4 only\n");*/
	}else if( ipv6Only ){
		sHints.ai_family = PF_INET6;
		/*printf("ipv6 only\n");*/
	}else{
		sHints.ai_family = PF_UNSPEC;
	}
	sHints.ai_socktype = SOCK_STREAM;
	sHints.ai_flags = AI_PASSIVE; /*for bind()*/
	sHints.ai_protocol = 0;
	rc = getaddrinfo(localOnly ? "localhost": 0, sPort, &sHints, &pAddrs);
	if( rc ){
		fprintf(stderr, "could not get addr info: %s",
            rc!=EAI_SYSTEM ? gai_strerror(rc) : strerror(errno));
		return 1;
	}


	for(n=0, p=pAddrs; n<(int)(sizeof(listener)/sizeof(listener[0])) && p!=0; p=p->ai_next){

		listener[n] = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if( listener[n]>=0 ){
			/* if we can't terminate nicely, at least allow the socket to be reused */
			setsockopt(listener[n], SOL_SOCKET, SO_REUSEADDR,&opt, sizeof(opt));
      
			#if defined(IPV6_V6ONLY)
			if( p->ai_family==AF_INET6 ){
				int v6only = 1;
				setsockopt(listener[n], IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
			}
			#endif
      
			if( bind(listener[n], p->ai_addr, p->ai_addrlen)<0 ){
				printf("bind failed: %s\n", strerror(errno));
				close(listener[n]);
				continue;
			}
			if( listen(listener[n], 20)<0 ){
				printf("listen() failed: %s\n", strerror(errno));
				close(listener[n]);
				continue;
			}
			n++;
		}

	}

	if( n==0 ){
		fprintf(stderr, "cannot open any sockets\n");
		return 1;
	}
	freeaddrinfo(pAddrs);


	while( 1 ){
		if( nchildren>MAX_PARALLEL ){
			/* Slow down if connections are arriving too fast */
			sleep( nchildren-MAX_PARALLEL );
		}

		delay.tv_sec = 60;
		delay.tv_usec = 0;
		FD_ZERO(&readfds);

		for(i=0; i<n; i++){
			assert( listener[i]>=0 );
			FD_SET( listener[i], &readfds);
			if( listener[i]>maxFd ) maxFd = listener[i];
		}

		select( maxFd+1, &readfds, 0, 0, &delay);
		for(i=0; i<n; i++){
			if( FD_ISSET(listener[i], &readfds) ){
				lenaddr = sizeof(inaddr);
				connection = accept(listener[i], &inaddr.sa, &lenaddr);
				if( connection>=0 ){
					child = fork();
					if( child!=0 ){
						if( child>0 ) nchildren++;
						close(connection);
						/* printf("subprocess %d started...\n", child); fflush(stdout); */
					}else{
						int nErr = 0, fd;
						close(0);
						fd = dup(connection);
						if( fd!=0 ) nErr++;
						close(1);
						fd = dup(connection);
						if( fd!=1 ) nErr++;
						close(connection);
						return nErr;
					}
				}
			}

			/* Bury dead children */
			while( (child = waitpid(0, 0, WNOHANG))>0 ){
				/* printf("process %d ends\n", child); fflush(stdout); */
				nchildren--;
			}
		}
	}




	/* NOT REACHED */  
	exit(1);
}


int processClient(const char *program)
{
	int px[2]; /*parent write to child pipe*/
	int py[2]; /*parent read from child pipe*/

	fd_set readfds, allset;
	struct timeval delay;
	int maxFd = -1;
	char sLine[1024];
	int nbyte,nready;
	int n,i;
	int rtn; 




    if( pipe(px)){
		printf("pipe faild\n");
		return -1;
    }

	if(pipe(py)){
		close(px[0]);
		close(px[1]);
		return -1;
	}

	if( fork()==0 ){

		close(0);
		close(px[1]);
		if( dup2(px[0], 0)!=0 ){
			fprintf(stderr,"error ...1\n");
		}
		close(px[0]);

        close(1);
		close(py[0]);
        if( dup2(py[1],1)!=1 ){
			fprintf(stderr,"dup err..\n");
        }
        close(py[1]);

        for(i=3; close(i)==0; i++){}
        execl(program, program, (char*)0);

		/*if execl fail...*/
        exit(errno);
	}


	/**parent**/
	close(px[0]);
	close(py[1]);

	delay.tv_sec = 3;
    delay.tv_usec = 0;

	FD_ZERO(&readfds);
	FD_ZERO(&allset);

	FD_SET(py[0], &allset);
    if(py[0] > maxFd) maxFd = py[0];

    FD_SET(0, &allset);
	if(0 > maxFd) maxFd = 0;
	
	while(1){

		memcpy(&readfds,&allset,sizeof(allset));

		nready = select( maxFd+1, &readfds, 0, 0, &delay);
		if((nready == -1) && (errno !=EINTR)) {
			break;
		}

		if( FD_ISSET(py[0], &readfds)) {
			nbyte = read(py[0],sLine,sizeof(sLine));
			if(nbyte <=0) {
				close(py[0]);
			}else {
				nbyte = write(1,sLine,nbyte);
			}
		}
		if( FD_ISSET(0, &readfds)) {
			nbyte = read(0,sLine,sizeof(sLine));
			if(nbyte <=0) {
                close(px[1]);
			}else{
				nbyte = write(px[1],sLine,nbyte);
			}
        }
	}

	wait(&rtn);
    fprintf(stderr, " child process return %d\n", rtn );
	return 0;
	
}



int main(int argc, char **argv){

    const char *sPort = 0;  
	const char *program = "./test.sh";

	sPort = DEFAULT_PORT;

    /* Activate the server, if requested */
    if( sPort && ws_server(sPort, 0) ){
        /* failed*/
    }


	/*in child do*/
	processClient(program);

    exit(0);
}

