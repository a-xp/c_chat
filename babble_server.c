#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <assert.h>

#include "babble_server.h"
#include "babble_types.h"
#include "babble_utils.h"
#include "babble_communication.h"
#include "thpool.h"

threadpool conn_workers_pool;
threadpool cmd_workers_pool;

static void display_help(char *exec)
{
    printf("Usage: %s -p port_number\n", exec);
}

int main(int argc, char *argv[])
{
    int sockfd, newsockfd;
    int portno=BABBLE_PORT;

    int opt;
    int nb_args=1;

    while ((opt = getopt (argc, argv, "+p:")) != -1){
        switch (opt){
        case 'p':
            portno = atoi(optarg);
            nb_args+=2;
            break;
        case 'h':
        case '?':
        default:
            display_help(argv[0]);
            return -1;
        }
    }
    
    if(nb_args != argc){
        display_help(argv[0]);
        return -1;
    }

    server_data_init();    
    conn_workers_pool = thpool_init(BABBLE_COMMUNICATION_THREADS);
    cmd_workers_pool = thpool_init(BABBLE_EXECUTOR_THREADS);

    if((sockfd = server_connection_init(portno)) == -1){
        return -1;
    }

    printf("Babble server bound to port %d\n", portno);    
    
    /* main server loop */
    while(1){

        if((newsockfd= server_connection_accept(sockfd))==-1){
            return -1;
        }
        session_t* newsession = (session_t *)malloc(sizeof(session_t));
        newsession->handle = newsockfd;
        thpool_add_work(conn_workers_pool, (void*)connection_listener, newsession);        
    }
    close(sockfd);
    return 0;
}
