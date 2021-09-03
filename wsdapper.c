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
#ifndef MAX_SOCKS_BUFFER
#define MTU 1250
#define MAX_SOCKS_BUFFER ((int)sysconf(_SC_PAGESIZE))
#endif
#ifndef REMOTE_CLOSED
#define REMOTE_CLOSED 0x101
#endif
#ifndef CONN_ESTABLISHED
#define CONN_ESTABLISHED 0
#endif

#define MAX(x,y) ({\
	__typeof__(x) _x = x; \
	__typeof__(y) _y = y; \
	(void) ( &_x == &_y ); \
	_x>_y ? _x : _y;\
})




/*
** We record most of the state information as global variables.  This
** saves having to pass information to subroutines as parameters, and
** makes the executable smaller...
*/
static char *zRoot = 0;          /* Root directory of the website */
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
**Write data to fd
**
**return if success 0 else return -1
*/
static int forward_to_fd(int fd,char *src,size_t len)
{
	int rc =0;

    size_t n = write(fd,src,len);

	if(n==-1 && !(errno == EAGAIN || errno == EWOULDBLOCK)){
			rc =-1;
	}

    return rc;
}


/*
** Implement an ws server daemon listening on port zPort.
**
** As new connections arrive, fork a child and let the child return
** out of this procedure call.  The child will handle the request.
** The parent never returns from this procedure.
**
** Return 0 to each child as it runs.  If unable to establish a
** listening socket, return non-zero.
*/
int ws_server(const char *zPort, int localOnly){
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
	rc = getaddrinfo(localOnly ? "localhost": 0, zPort, &sHints, &pAddrs);
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
					}else{ /*child*/
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

/*
**websocket  has two parts: a handshake and the data transfer.
**
**The handshake from the client looks as follows:
**
**       GET /chat HTTP/1.1
**        Host: server.example.com
**        Upgrade: websocket
**        Connection: Upgrade
**        Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
**        Origin: http://example.com
**        Sec-WebSocket-Protocol: chat, superchat
**        Sec-WebSocket-Version: 13
**
**
**The handshake from the server looks as follows:
**
**        HTTP/1.1 101 Switching Protocols
**        Upgrade: websocket
**        Connection: Upgrade
**        Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
**        Sec-WebSocket-Protocol: chat
**
**This routine processes a websocket request on standard input and
** sends the reply to standard output.
**return: if error return -1 else return 0
**when success, zOut is will exec program
*/
int opening_handshaker(char *zOut)
{
	char zLine[1024];
	/* Get the first line of the request and parse out the
	** method, the script and the protocol.
	*/
	if( fgets(zLine,sizeof(zLine),stdin)==0 ){
		exit(0);
	}

}

/*
** This routine processes a single websocket request on standard input and
** sends the reply to standard output.  
**
** If the connection should be closed, this routine calls exit() and
** thus never returns.  If this routine does return it means that another
** websocket request may appear on the wire.
*/

int process_one_client(char **program)
{
	int pipe_write_fd[2]; /*parent write to child(program) data pipe*/
	int pipe_read_fd[2];  /*parent read from child(program) data pipe*/

	int socket_read_fd =fileno(stdin); /*read from stdin*/
	int socket_write_fd =fileno(stdout); /*write to stdout*/

	fd_set readfds,dumpfds;
    struct timeval tv;
    int nfds =0;
    int nready;
    int status=REMOTE_CLOSED;
    char socks_buffer[MAX_SOCKS_BUFFER];
	int rtn;


	do{

		/*pipe create*/

		if(pipe(pipe_write_fd)){
			/*log */
			break;
		}

		if(pipe(pipe_read_fd)){
			/*log*/
			close(pipe_write_fd[0]);
			close(pipe_write_fd[1]);
			break;
		}
		

		/*fork child exec program*/

		if( fork()==0 ){/*in child*/
			/*parent use pipe_write_fd[1]  -> write to -> child, means child from pipe_write_fd[0] read data*/
			close(0);
			close(pipe_write_fd[1]);
			if( dup2(pipe_write_fd[0], 0)!=0 ){
				/*log*/
				exit(errno);
			}
			close(pipe_write_fd[0]);

			/*parent use pipe_read_fd[0]  <- read data from child, means child wirte data to  pipe_read_fd[1]*/

			close(1);
			close(pipe_read_fd[0]);
			if( dup2(pipe_read_fd[1],1)!=1 ){
				/*log*/
				exit(errno);
			}
			close(pipe_read_fd[1]);

			int i;
			for( i=3; close(i)==0; i++){}
			execvp(program[0],program);

			/*if execl fail...*/
			fprintf(stderr,"child will exit .....\n");
			exit(errno);
		}

		/*in parent close */
		close(pipe_read_fd[1]);
		close(pipe_write_fd[0]);
		status = CONN_ESTABLISHED;

	}while(0);


	/* int parent
     * wait for sockets to become readable and forward data
     *
     */
	tv.tv_sec = 1000;
	tv.tv_usec = 0;

    FD_ZERO(&dumpfds);

    FD_SET(pipe_read_fd[0], &dumpfds);
    nfds = MAX(nfds, pipe_read_fd[0]);

    FD_SET(socket_read_fd, &dumpfds);
    nfds = MAX(nfds, socket_read_fd);

	while(status != REMOTE_CLOSED){

		do{
			memcpy(&readfds,&dumpfds,sizeof(dumpfds));

		}while ( ( nready=select(nfds+1, &readfds, NULL, NULL, &tv) ) == -1 && errno == EINTR);

		/*read from pipe */
        if (FD_ISSET(pipe_read_fd[0], &readfds)) {
			ssize_t rret;
			while ((rret = read(pipe_read_fd[0], socks_buffer, sizeof(socks_buffer))) == -1 && errno == EINTR)
                ;
			if(rret ==0 || rret ==-1) {
				/*pipe close*/
				close(pipe_read_fd[0]);
				close(socket_write_fd);
				status =REMOTE_CLOSED; 
			}else{
				if(forward_to_fd(socket_write_fd,socks_buffer,rret)==-1){
					close(pipe_read_fd[0]);
					close(socket_write_fd);
					status =REMOTE_CLOSED;
				}

			}
		}

		/*read from sock*/
		if (FD_ISSET(socket_read_fd, &readfds)) {
			ssize_t rret;
            while ((rret = read(socket_read_fd, socks_buffer, sizeof(socks_buffer))) == -1 && errno == EINTR)
                ;
            if(rret ==0 || rret ==-1) {
                /*fd close*/
				close(socket_read_fd);
				close(pipe_write_fd[1]);
				status =REMOTE_CLOSED;
            }else{
               if(forward_to_fd(pipe_write_fd[1],socks_buffer,rret)==-1){
				   close(socket_read_fd);
	               close(pipe_write_fd[1]);
		           status =REMOTE_CLOSED;
			   }
            }
        }

	}

	wait(&rtn);
	/*log: request close*/
    return 0;

}



/*
** $ wsdapper --port=8080 my-program
*/
int main(int argc, char **argv){

    const char *zPort = 0;  
	const char *zStaticDir=0;
	char **program=0;

	zPort = DEFAULT_PORT;

	/* Parse command-line arguments
	*/
	while( argc>1 && argv[1][0]=='-' ){
		char *z = argv[1];
		char *zArg = argc>=3 ? argv[2] : "0";	
		if( z[0]=='-' && z[1]=='-' ) z++;
		
		if( strcmp(z,"-port")==0 ){
			zPort = zArg;
			fprintf(stderr,"sport = %s\n",zPort);

        }else if(strcmp(z,"-static")==0){
			zStaticDir = zArg;			
			fprintf(stderr,"static dir = %s\n",zStaticDir);

		}else{
			fprintf(stderr,"unknown argument: [%s]\n", z);
			exit(0);
		}
		argv += 2;
		argc -= 2;
	}

	if(argc>1 && *argv){
		program = ++argv;
		char **p=program;

		fprintf(stderr,"will exec program[");
		while(*p){
			fprintf(stderr," %s",*p++);
		}
		fprintf(stderr,"]\n");


	}else{
		fprintf(stderr,"unknown exec program(NULL)\n");
		exit(0);
	}


    /* Activate the server, if requested */
    if( zPort && ws_server(zPort, 0) ){
        /* failed*/
    }


	/*in child do*/
	process_one_client(program);

    exit(0);
}

