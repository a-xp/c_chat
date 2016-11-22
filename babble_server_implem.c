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
#include "thpool.h"

time_t server_start;

//threadpool cmd_executors_pool;

/* used to create a timeline of publications*/
typedef struct timeline_item{
    publication_t *pub;   /* the publication */
    client_data_t *client;   /* publication author */
    struct timeline_item *next;
} timeline_item_t;


/* freeing client_data_t struct */
static void free_client_data(client_data_t *client)
{
    if(client == NULL){
        return;
    }


    /* IMPORTANT: we choose not to free client_data_t structures when
     * a client disconnects. The reason is that pointers to this data
     * structure are stored if several places in the code, and so,
     * freeing properly would be a complex operation */
    
    /*publication_t *item= client->pub_list, *previous;

    while(item!=NULL){
        previous=item;
        item = item->next;
        free(previous);
    }

    free(client);*/
}

/* stores an error message in the answer_set of a command */
void generate_cmd_error(command_t *cmd)
{
    /* lookup client */
    client_data_t *client = registration_lookup(cmd->key);

    

    if(client == NULL){
        fprintf(stderr, "Error -- no client found\n");
        return;
    }

    cmd->answer.size = -1;
    cmd->answer.aset = malloc(sizeof(answer_t));

    if(cmd->cid == LOGIN || cmd->cid == PUBLISH || cmd->cid == FOLLOW){
        snprintf(cmd->answer.aset->msg, BABBLE_BUFFER_SIZE,"%s[%ld]: ERROR -> %d { %s } \n", client->client_name, time(NULL)-server_start, cmd->cid, cmd->msg);
    }
    else{
        snprintf(cmd->answer.aset->msg, BABBLE_BUFFER_SIZE,"%s[%ld]: ERROR -> %d \n", client->client_name, time(NULL)-server_start, cmd->cid);

    }
}

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



int run_login_command(command_t *cmd)
{
    struct timespec tt;
    clock_gettime(CLOCK_REALTIME, &tt);
    
    /* compute hash of the new client id */
    cmd->key = hash(cmd->msg);
    
    client_data_t *client_data=malloc(sizeof(client_data_t));
    
    strncpy(client_data->client_name, cmd->msg, BABBLE_ID_SIZE);
    client_data->sock = cmd->sock;
    client_data->key=cmd->key;
    client_data->pub_set=publication_set_create();
    client_data->last_timeline=(uint64_t)1000000000 * tt.tv_sec + tt.tv_nsec;
    /* by default, we follow ourself*/
    client_data->followed[0]=client_data;
    client_data->nb_followed=1;
    client_data->nb_follower=1;

    if(registration_insert(client_data)){
        free(client_data->pub_set);
        free(client_data);
        generate_cmd_error(cmd);
        return -1;
    }
    
    printf("### New client %s (key = %lu)\n", client_data->client_name, client_data->key);

    /* answer to client */
    cmd->answer.size = -1;
    cmd->answer.aset = malloc(sizeof(answer_t));
    
    snprintf(cmd->answer.aset->msg, BABBLE_BUFFER_SIZE,"%s[%ld]: registered with key %lu\n", client_data->client_name, tt.tv_sec - server_start, client_data->key);
    
    return 0;
}


int run_publish_command(command_t *cmd)
{    
    client_data_t *client = registration_lookup(cmd->key);
    
    if(client == NULL){
        fprintf(stderr, "Error -- no client found\n");
        generate_cmd_error(cmd);
        return -1;
    }
    
    publication_t *pub = publication_set_insert(client->pub_set, cmd->msg);
    
    printf("### Client %s published { %s } at date %ld\n", client->client_name, pub->msg, pub->date);

    /* answer to client */
    cmd->answer.size = -1;
    cmd->answer.aset = malloc(sizeof(answer_t));

    snprintf(cmd->answer.aset->msg, BABBLE_BUFFER_SIZE,"%s[%ld]: { %s }\n", client->client_name, pub->date, pub->msg);
    
    return 0;
}


int run_follow_command(command_t *cmd)
{
    client_data_t *client = registration_lookup(cmd->key);
    
    if(client == NULL){
        fprintf(stderr, "Error -- no client found\n");
        generate_cmd_error(cmd);
        return -1;
    }

    /* compute hash of the client to follow */
    unsigned long f_key = hash(cmd->msg);

    /* lookup client to follow */
    client_data_t *f_client = registration_lookup(f_key);
    
    if(f_client == NULL){
        generate_cmd_error(cmd);        
        return 0;
    }
    
    /* if client is not already followed, add it*/
    int i=0;

    for(i=0; i<client->nb_followed; i++){
        if(client->followed[i]->key == f_key){
            break;
        }
    }
    
    if(i == client->nb_followed){
        client->followed[i]=f_client;
        client->nb_followed++;
        f_client->nb_follower++;
    }

    
    /* answer to client */
    cmd->answer.size = -1;
    cmd->answer.aset = malloc(sizeof(answer_t));

    snprintf(cmd->answer.aset->msg, BABBLE_BUFFER_SIZE,"%s[%ld]: follow %s\n", client->client_name, time(NULL)-server_start, f_client->client_name);

    return 0;
}


int run_timeline_command(command_t *cmd)
{
    int i=0;
    timeline_item_t *pub_list=NULL;
    int item_count=0;
    
    /* get current time to know up to when we publish*/
    struct timespec tt;
    clock_gettime(CLOCK_REALTIME, &tt);

    uint64_t end_time= (uint64_t)1000000000 * tt.tv_sec + tt.tv_nsec;

    /* lookup client */
    client_data_t *client = registration_lookup(cmd->key);

    if(client == NULL){
        fprintf(stderr, "Error -- no client found\n");
        generate_cmd_error(cmd);
        return -1;
    }

    /* start from where we finished last time*/
    uint64_t start_time=client->last_timeline;
    
    /* gather publications over all followed clients */
    for(i=0; i < client->nb_followed; i++){
        client_data_t *f_client=client->followed[i];
        publication_t *pub=NULL;

        timeline_item_t *time_iter=pub_list, *prev;
        /* add recent items to the totally ordered list */
        while((pub = publication_set_getnext(f_client->pub_set, pub, start_time)) != NULL){
            /* if publication is more recent than the timeline
               command, we do not consider it */
            if(pub->ndate > end_time){
                break;
            }
            
            timeline_item_t *item=malloc(sizeof(timeline_item_t));
            item->pub=pub;
            item->client= f_client;
            item->next = NULL;

            while(time_iter!=NULL && time_iter->pub->date < pub->date){
                prev = time_iter;
                time_iter = time_iter->next;
            }

            /* insert in first position */
            if(time_iter == pub_list){
                pub_list = item;
                item->next=time_iter;
            }
            else{
                item->next=time_iter;
                prev->next=item;
            }
            item_count++;
            time_iter = item;
        }
    }


    /* now that we have a full timeline, we generate the messages to
     * the client */
    
    /* save number of items to transmit */
    cmd->answer.size = item_count;
    
    /* generate each item*/
    answer_t *current_answer=NULL;
    timeline_item_t *time_iter=pub_list;
    while(time_iter != NULL){
        if(current_answer == NULL){
            current_answer = malloc(sizeof(answer_t));
            current_answer->next=NULL;
            cmd->answer.aset = current_answer;
        }
        else{
            current_answer->next = malloc(sizeof(answer_t));
            current_answer = current_answer->next;
            current_answer->next=NULL;
        }
                
        snprintf(current_answer->msg, BABBLE_BUFFER_SIZE,"    %s[%ld]: %s\n", time_iter->client->client_name, time_iter->pub->date, time_iter->pub->msg);
    
        time_iter = time_iter->next;
    }

    client->last_timeline = end_time;
    
    return 0;
}


int run_fcount_command(command_t *cmd)
{
    /* lookup client */
    client_data_t *client = registration_lookup(cmd->key);

    if(client == NULL){
        fprintf(stderr, "Error -- no client found\n");
        generate_cmd_error(cmd);
        return -1;
    }
    
    /* answer to client */
    cmd->answer.size = -1;
    cmd->answer.aset = malloc(sizeof(answer_t));
    
    snprintf(cmd->answer.aset->msg, BABBLE_BUFFER_SIZE,"%s[%ld]: has %d followers\n", client->client_name, time(NULL) - server_start, client->nb_follower);
    
    return 0;
}

int run_rdv_command(command_t *cmd)
{
    /* lookup client */
    client_data_t *client = registration_lookup(cmd->key);
    
    if(client == NULL){
        fprintf(stderr, "Error -- no client found\n");
        generate_cmd_error(cmd);
        return -1;
    }
    
    /* answer to client */
    cmd->answer.size = -1;
    cmd->answer.aset = malloc(sizeof(answer_t));
    
    snprintf(cmd->answer.aset->msg, BABBLE_BUFFER_SIZE,"%s[%ld]: rdv_ack\n", client->client_name, time(NULL) - server_start);
    
    return 0;
}


int unregisted_client(command_t *cmd)
{
    assert(cmd->cid == UNREGISTER);
    
    /* remove client */
    client_data_t *client = registration_remove(cmd->key);

    if(client != NULL){
        printf("### Unregister client %s (key = %lu)\n", client->client_name, client->key);

        free_client_data(client);
    }
    

    return 0;
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


static int process_command(command_t *cmd)
{
    int res=0;

    switch(cmd->cid){
    case LOGIN:
        res = run_login_command(cmd);
        break;
    case PUBLISH:
        res = run_publish_command(cmd);
        break;
    case FOLLOW:
        res = run_follow_command(cmd);
        break;
    case TIMELINE:
        res = run_timeline_command(cmd);
        break;
    case FOLLOW_COUNT:
        res = run_fcount_command(cmd);
        break;
    case RDV:
        res = run_rdv_command(cmd);
        break;
    default:
        fprintf(stderr,"Error -- Unknown command id\n");
        return -1;
    }

    if(res){
        fprintf(stderr,"Error -- Failed to run command ");
        display_command(cmd, stderr);
    }

    return res;
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


void connection_listener(int* fd_p)
{
    char* recv_buff=NULL;
    int recv_size=0;
    int newsockfd = *fd_p;

    command_t *cmd;
    unsigned long client_key=0;
    char client_name[BABBLE_ID_SIZE+1];
    
        bzero(client_name, BABBLE_ID_SIZE+1);
        if((recv_size = network_recv(newsockfd, (void**)&recv_buff)) < 0){
            fprintf(stderr, "Error -- recv from client\n");
            close(newsockfd);
            return;
        }

        cmd = new_command(0);
        
        if(parse_command(recv_buff, cmd) == -1 || cmd->cid != LOGIN){
            fprintf(stderr, "Error -- in LOGIN message\n");
            close(newsockfd);
            free(cmd);
            return;
        }

        /* before processing the command, we should register the
         * socket associated with the new client; this is to be done only
         * for the LOGIN command */
        cmd->sock = newsockfd;
    
        if(process_command(cmd) == -1){
            fprintf(stderr, "Error -- in LOGIN\n");
            close(newsockfd);
            free(cmd);
            return;    
        }

        /* notify client of registration */
        if(answer_command(cmd) == -1){
            fprintf(stderr, "Error -- in LOGIN ack\n");
            close(newsockfd);
            free(cmd);
            return;
        }

        /* let's store the key locally */
        client_key = cmd->key;

        strncpy(client_name, cmd->msg, BABBLE_ID_SIZE);
        free(recv_buff);
        free(cmd);

        /* looping on client commands */
        while((recv_size=network_recv(newsockfd, (void**) &recv_buff)) > 0){
            cmd = new_command(client_key);
            if(parse_command(recv_buff, cmd) == -1){
                fprintf(stderr, "Warning: unable to parse message from client %s\n", client_name);
                notify_parse_error(cmd, recv_buff);
            }
            else{
                if(process_command(cmd) == -1){
                    fprintf(stderr, "Warning: unable to process command from client %lu\n", client_key);
                }
                if(answer_command(cmd) == -1){
                    fprintf(stderr, "Warning: unable to answer command from client %lu\n", client_key);
                }
            }
            free(recv_buff);
            free(cmd);
        }

        if(client_name[0] != 0){
            cmd = new_command(client_key);
            cmd->cid= UNREGISTER;
            
            if(unregisted_client(cmd)){
                fprintf(stderr,"Warning -- failed to unregister client %s\n",client_name);
            }
            free(cmd);
        } 
    
}

void cmd_executor(command_t* cmd)
{
    
}


