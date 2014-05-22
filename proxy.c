/*
 * proxy.c - CS:APP Web proxy
 *
 * TEAM MEMBERS:
 *     Kat Winter, katjwinter@gmail.com
 * 
 * Multi-threaded proxy server.
 * Creates a new thread for each connection and that thread
 * then proxies between the client and the web server for the site
 * being requested.
 */ 

#include "csapp.h"
#include "signal.h"
/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, int  *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size);
void run_proxy(int connfd, int tNum, struct sockaddr_in clientaddr);
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen);
void Rio_writen_w(int fd, void *usrbuf, size_t n);
ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t maxlen);
int open_clientfd_ts(char *hostname, int port);
void *thread(void *vargp);

/* information that each thread will need */
typedef struct {
  int tNum;
  int connfd;
  struct sockaddr_in tclientaddr;
}threadArgs_str;

/* semaphores */
sem_t ocMutex, logMutex, stdoutMutex;

/* 
 * main - Main routine for the proxy program 
 */
int main(int argc, char **argv)
{  
  int listenfd, port;
  int threadCount = 0;
  socklen_t clientlen=sizeof(struct sockaddr_in);
  struct sockaddr_in clientaddr;
  pthread_t tid;

  signal(SIGPIPE, SIG_IGN);

    /* Check arguments */
    if (argc != 2) {
	fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
	exit(0);
    }

    /* initialize semaphores */
    Sem_init(&ocMutex, 0, 1);
    Sem_init(&logMutex, 0, 1);
    Sem_init(&stdoutMutex, 0, 1);

    port = atoi(argv[1]);

    listenfd = Open_listenfd(port);
    while (1) {
      threadArgs_str *threadArg_p = (threadArgs_str *)Malloc(sizeof(threadArgs_str));
      clientlen = sizeof(threadArg_p->tclientaddr);
      threadArg_p->connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
      threadArg_p->tNum = threadCount++;
      threadArg_p->tclientaddr = clientaddr;
      Pthread_create(&tid, NULL, thread, threadArg_p);
    }

    exit(0);
}

/* Thread routine */
void *thread(void *vargp) {
  threadArgs_str threadargs = *((threadArgs_str *)vargp);
  Pthread_detach(pthread_self());
  Free(vargp);
  run_proxy(threadargs.connfd, threadargs.tNum, threadargs.tclientaddr);
  Close(threadargs.connfd);
  return NULL;
}

/*
 * Run the proxy server
 */
void run_proxy(int connfd, int tNum, struct sockaddr_in clientaddr) {
  
  char requestbuf[MAXLINE], headerbuftmp[MAXLINE], headerbuf[MAXBUF*4];
  char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char hostname[MAXLINE], pathname[MAXLINE];
  int port;
  int webfd;
  rio_t rio_client;
  rio_t rio_web;

  /* read request line into requestbuf */
  Rio_readinitb(&rio_client, connfd);
  Rio_readlineb_w(&rio_client, requestbuf, MAXLINE);
 
  sscanf(requestbuf, "%s %s %s", method, uri, version); // parse into three strings
  if (strcasecmp(method, "GET")) {
    P(&stdoutMutex);
    printf("Only GET is implemented\n");
    V(&stdoutMutex);
    return;
  }
  
  /* read headers into headerbuf */
  Rio_readlineb_w(&rio_client, headerbuftmp, MAXLINE);
  strcat(headerbuf, headerbuftmp);
  while(strcmp(headerbuftmp, "\r\n")) {
    Rio_readlineb_w(&rio_client, headerbuftmp, MAXLINE);
    if ((strcmp(headerbuftmp, "\r\n") == 0)) {
      strcat(headerbuf, "Connection: close\n"); // don't support keep alive
      }
    strcat(headerbuf, headerbuftmp);
  }
  
  /* parse URI */
  if ((parse_uri(uri, hostname, pathname, &port)) < 0) {
    P(&stdoutMutex);
    printf("error parsing URI\n");
    V(&stdoutMutex);
  }

  P(&stdoutMutex);
  printf("Thread %d received request from %s:\n%s\n%s", tNum, hostname, requestbuf, headerbuf);
  printf("*** End of Request ***\n\n");
  V(&stdoutMutex);

  /* recreate request from proxy request to web server request */
  char webrequestbuf[MAXLINE*3] = "GET /";
  strcat(webrequestbuf, pathname);
  strcat(webrequestbuf, " ");
  strcat(webrequestbuf, version);
  strcat(webrequestbuf, "\r\n");

  P(&stdoutMutex);
  printf("Thread %d forwarding request to web server:\n%s\n%s", tNum, webrequestbuf, headerbuf);
  printf("*** End of Request ***\n\n");
  V(&stdoutMutex);

  /* connect to actual web server */
  webfd = open_clientfd_ts(hostname, port);

  /* send request and headers */
  Rio_writen_w(webfd, webrequestbuf, strlen(webrequestbuf));
  Rio_writen_w(webfd, headerbuf, strlen(headerbuf));

  char bodybuf[MAXBUF];
  int n;

  /* read from web server and send to client */
  Rio_readinitb(&rio_web, webfd);
  int bytessent = 0;
  while ((n = Rio_readnb_w(&rio_web, bodybuf, MAXBUF)) != 0) {
    Rio_writen_w(connfd, bodybuf, n);
    bytessent += n;
  }
  
  Close(webfd);

  P(&stdoutMutex);
  printf("Thread %d sent %d bytes from web server to client\n", tNum, bytessent);
  V(&stdoutMutex);

  char logEntry[MAXLINE];
  format_log_entry(logEntry, &clientaddr, uri, bytessent);
  FILE *logFile;
  logFile = fopen("proxylog.log", "a+");
  
  /* print to the log (mutually excluded) */
  P(&logMutex);
  int logBytes = fwrite(logEntry, sizeof(char), strlen(logEntry), logFile);
  if (logBytes != strlen(logEntry)) {
     printf("error writing log file\n");
  }
  fclose(logFile);
  V(&logMutex);
}

/*
 * parse_uri - URI parser
 * 
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, int *port)
{
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
    	hostname[0] = '\0';
    	return -1;
    }

    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';

    /* Extract the port number */
    *port = 80; /* default */
    if (*hostend == ':')   
	*port = atoi(hostend + 1);

    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
	pathname[0] = '\0';
    }
    else {
	pathbegin++;	
	strcpy(pathname, pathbegin);
    }

    return 0;
}

/**********************************
 * Wrappers for robust I/O routines
 **********************************/

ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n) 
{
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0) {
      printf("Rio_readnb error\n");
      return 0;
    }
    return rc;
}

void Rio_writen_w(int fd, void *usrbuf, size_t n) 
{
  if (rio_writen(fd, usrbuf, n) != n) {
    printf("Rio_writen error\n");
  }
}

ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen) 
{
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0) {
      printf("Rio_readlineb error\n");
      return 0;
    }

    return rc;
} 

/* threadsafe version of open_clientfd */
int open_clientfd_ts(char *hostname, int port) 
{
    int clientfd;
    struct hostent hh;
    struct hostent *hp = &hh;
    struct hostent *copyhp;
    struct sockaddr_in serveraddr;

    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	return -1; /* check errno for cause of error */
    
    /* lock, copy, then unlock */
    P(&ocMutex);
    if ((copyhp = gethostbyname(hostname)) != NULL) {
      hh = *copyhp;
    }
    V(&ocMutex);

    if (copyhp == NULL) {
      return -2;
    }

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0], 
	  (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);

    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
	return -1;
    return clientfd;
}

/*
 * format_log_entry - Create a formatted log entry in logstring. 
 * 
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size in bytes
 * of the response from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, 
		      char *uri, int size)
{
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /* 
     * Convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 13, CS:APP).
     */
    host = ntohl(sockaddr->sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;


    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %d.%d.%d.%d %s %d\n", time_str, a, b, c, d, uri, size);
}


