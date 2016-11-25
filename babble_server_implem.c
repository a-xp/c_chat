#include <stdio.h>
#include <unistd.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "babble_server.h"
#include "babble_utils.h"
#include "babble_types.h"
#include "babble_communication.h"
#include "babble_registration.h"
#include "thread_pool.h"
#include "babble_commands.h"

time_t server_start;

//threadpool cmd_executors_pool;


/* can be used to display the content of a command */
void display_command(command_t *cmd, FILE* stream)
{
    switch(cmd->cid){
    case LOGIN:
        fprintf(stream,"LOGIN: %s\n", cmd->msg);
        break;
    case PUBLISH:
        fprintf(stream,"PUBLISH: %s\n", cmd->msg);
        break;
    case FOLLOW:
        fprintf(stream,"FOLLOW: %s\n", cmd->msg);
        break;
    case TIMELINE:
        fprintf(stream,"TIMELINE\n");
        break;
    case FOLLOW_COUNT:
        fprintf(stream,"FOLLOW_COUNT\n");
        break;
    case RDV:
        fprintf(stream,"RDV\n");
        break;
    default:
        fprintf(stream,"Error -- Unknown command id\n");
        return;
    }
}

/* initialize the server */
void server_data_init(void)
{
    server_start = time(NULL);
    registration_init();
    //cmd_executors_pool = thpool_init(4);    
}

/* open a socket to receive client connections */
int server_connection_init(int port)
{
    int sockfd;
    struct sockaddr_in serv_addr;
    int reuse_opt=1;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0){
        perror("ERROR opening socket");
        return -1;
    }

    if(setsockopt(sockfd, SOL_SOCKET, (SO_REUSEPORT | SO_REUSEADDR), (void*)&reuse_opt, sizeof(reuse_opt)) < 0){
        perror("setsockopt failed\n");
        close(sockfd);
        return -1;
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0){
        perror("ERROR on binding");
        close(sockfd);
        return -1;
    }
    
    if(listen(sockfd, BABBLE_BACKLOG)){
        perror("ERROR on listen");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* accept connections of the server socket and return corresponding
 * new file descriptor */
int server_connection_accept(int sock)
{
    int new_sock;
    struct sockaddr_in cli_addr;
    socklen_t clilen = sizeof(cli_addr);

    new_sock = accept(sock, (struct sockaddr *) &cli_addr, &clilen);
    
    if (new_sock < 0){
        perror("ERROR on accept");
        close(sock);
        return -1;
    }
    
    return new_sock;
}


/* create a new command for client corresponding to key */
command_t* new_command(unsigned long key)
{
    command_t *cmd = malloc(sizeof(command_t));
    cmd->key = key;
    cmd->answer.size=-2;
    cmd->answer.aset=NULL;
    cmd->answer_exp=0;

    return cmd;
}

/* send error msg to client in case the input msg could not be parsed */
int notify_parse_error(command_t *cmd, char *input)
{
    /* lookup client */
    client_data_t *client = registration_lookup(cmd->key);

    if(client == NULL){
        fprintf(stderr, "Error -- no client found\n");
        return -1;
    }


    if(cmd->answer_exp){
        char buffer[BABBLE_BUFFER_SIZE];
        
        snprintf(buffer, BABBLE_BUFFER_SIZE,"%s[%ld]: ERROR -> %s\n", client->client_name, time(NULL)-server_start, input);
    
        if(write_to_client(cmd->key, strlen(buffer)+1, buffer)){
            fprintf(stderr,"Error -- could not send error msg: %s\n", buffer);
            return -1;
        }
    }
    
    return 0;
}


/* send buf to client identified by key */
int write_to_client(unsigned long key, int size, void* buf)
{
    client_data_t *client = registration_lookup(key);

    if(client == NULL){
        fprintf(stderr, "Error -- writing to non existing client %lu\n", key);
        return -1;
    }
    
    int write_size = network_send(client->sock, size, buf);
            
    if (write_size < 0){
        perror("writing to socket");
        return -1;
    }

    return 0;
}


static int parse_command(char* str, command_t *cmd)
{
    /* start by cleaning the input */
    str_clean(str);
    
    /* get command id */
    cmd->cid=str_to_command(str, &cmd->answer_exp);

    /* initialize other fields */
    cmd->answer.size=-1;
    cmd->answer.aset=NULL;

    switch(cmd->cid){
    case LOGIN:
        if(str_to_payload(str, cmd->msg, BABBLE_ID_SIZE)){
            fprintf(stderr,"Error -- invalid LOGIN -> %s\n", str);
            return -1;
        }
        break;
    case PUBLISH:
        if(str_to_payload(str, cmd->msg, BABBLE_SIZE)){
            fprintf(stderr,"Warning -- invalid PUBLISH -> %s\n", str);
            return -1;
        }
        break;
    case FOLLOW:
        if(str_to_payload(str, cmd->msg, BABBLE_ID_SIZE)){
            fprintf(stderr,"Warning -- invalid FOLLOW -> %s\n", str);
            return -1;
        }
        break;
    case TIMELINE:
        cmd->msg[0]='\0';
        break;
    case FOLLOW_COUNT:
        cmd->msg[0]='\0';
        break;
    case RDV:
        cmd->msg[0]='\0';
        break;    
    default:
        fprintf(stderr,"Error -- invalid client command -> %s\n", str);
        return -1;
    }

    return 0;
}

/* sends an answer for the command to the client if needed */
/* answer to a command is stored in cmd->answer after the command has
 * been processed. They are different cases
 + The client does not expect any answer (then nothing is sent)
 + The client expect an answer -- 2 cases
  -- The answer is a single msg
  -- The answer is potentially composed of multiple msgs (case of a timeline)
*/
static int answer_command(command_t *cmd)
{    
    /* case of no answer requested by the client */
    if(!cmd->answer_exp){
        if(cmd->answer.aset != NULL){
            free(cmd->answer.aset);
        }
        return 0;
    }
    
    /* no msg to be sent */
    if(cmd->answer.size == -2){
        return 0;
    }

    /* a single msg to be sent */
    if(cmd->answer.size == -1){
        /* strlen()+1 because we want to send '\0' in the message */
        if(write_to_client(cmd->key, strlen(cmd->answer.aset->msg)+1, cmd->answer.aset->msg)){
            fprintf(stderr,"Error -- could not send ack for %d\n", cmd->cid);
            free(cmd->answer.aset);
            return -1;
        }
        free(cmd->answer.aset);
        return 0;
    }
    

    /* a set of msgs to be sent */
    /* number of msgs sent first */
    if(write_to_client(cmd->key, sizeof(int), &cmd->answer.size)){
        fprintf(stderr,"Error -- send set size: %d\n", cmd->cid);
        return -1;
    }

    answer_t *item = cmd->answer.aset, *prev;
    int count=0;

    /* send only the last BABBLE_TIMELINE_MAX */
    int to_skip= (cmd->answer.size > BABBLE_TIMELINE_MAX)? cmd->answer.size - BABBLE_TIMELINE_MAX : 0;

    for(count=0; count < to_skip; count++){
        prev=item;
        item = item->next;
        free(prev);
    }
    
    while(item != NULL ){
        if(write_to_client(cmd->key, strlen(item->msg)+1, item->msg)){
            fprintf(stderr,"Error -- could not send set: %d\n", cmd->cid);
            return -1;
        }
        prev=item;
        item = item->next;
        free(prev);
        count++;
    }

    assert(count == cmd->answer.size);
    return 0;
}


void connection_listener(session_t* sess)
{
    char* recv_buff=NULL;
    int recv_size=0;
            
    fprintf(stderr, "Got request\n");
            
    command_t *cmd;
    unsigned long client_key=0;
    char client_name[BABBLE_ID_SIZE+1];
    
    bzero(client_name, BABBLE_ID_SIZE+1);
    if((recv_size = network_recv(sess->handle, (void**)&recv_buff)) < 0){
        fprintf(stderr, "Error -- recv from client\n");
        close(sess->handle);
        free(sess);
        return;
    }
    cmd = new_command(0);
    
    if(parse_command(recv_buff, cmd) == -1 || cmd->cid != LOGIN){
        fprintf(stderr, "Error -- in LOGIN message\n");
        close(sess->handle);
        free(cmd);
        free(sess);
        return;
    }

    /* before processing the command, we should register the
     * socket associated with the new client; this is to be done only
     * for the LOGIN command */
    cmd->sock = sess->handle;

    if(process_command(cmd) == -1){
        fprintf(stderr, "Error -- in LOGIN\n");
        close(sess->handle);
        free(cmd);
        free(sess);
        return;    
    }

    /* notify client of registration */
    if(answer_command(cmd) == -1){
        fprintf(stderr, "Error -- in LOGIN ack\n");
        close(sess->handle);
        free(cmd);
        free(sess);
        return;
    }
    
    /* let's store the key locally */
    client_key = cmd->key;

    strncpy(client_name, cmd->msg, BABBLE_ID_SIZE);
    free(recv_buff);
    free(cmd);

    /* looping on client commands */
    while((recv_size=network_recv(sess->handle, (void**) &recv_buff)) > 0){
        cmd = new_command(client_key);
        if(parse_command(recv_buff, cmd) == -1){
            fprintf(stderr, "Warning: unable to parse message from client %s\n", client_name);
            notify_parse_error(cmd, recv_buff);
            free(cmd);
        }
        else{
            thread_pool_submit(cmd_workers_pool, (void*)cmd_executor, cmd); 
        }
        free(recv_buff);
    }

    if(client_name[0] != 0){
        cmd = new_command(client_key);
        cmd->cid= UNREGISTER;
        
        if(unregisted_client(cmd)){
            fprintf(stderr,"Warning -- failed to unregister client %s\n",client_name);
        }
        free(cmd);
    } 
        
    free(sess);    
}

void cmd_executor(command_t* cmd)
{
    unsigned long client_key = cmd->key;
    if(process_command(cmd) == -1){
        fprintf(stderr, "Warning: unable to process command from client %lu\n", client_key);
    }
    if(answer_command(cmd) == -1){
        fprintf(stderr, "Warning: unable to answer command from client %lu\n", client_key);
    }  
    free(cmd);
}


