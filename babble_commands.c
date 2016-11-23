#include <stdio.h>
#include <unistd.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "babble_server.h"
#include "babble_utils.h"
#include "babble_types.h"
#include "babble_communication.h"
#include "babble_registration.h"
#include "babble_commands.h"

int process_command(command_t *cmd)
{
    int res=0;
    lock_client_data();
    
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
        unlock_client_data();
        return -1;
    }
    
    unlock_client_data();

    if(res){
        fprintf(stderr,"Error -- Failed to run command ");
        display_command(cmd, stderr);
    }

    return res;
}


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
        printf("### Client %s followed %s\n", client->client_name, f_client->client_name);
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
            printf("### Client %s got publication { %s }\n", client->client_name, pub->msg);
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
