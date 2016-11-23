#include <stdio.h>
#include <strings.h>
#include <pthread.h>

#include "babble_registration.h"

client_data_t *registration_table[MAX_CLIENT];
int nb_registered_clients;
pthread_mutex_t registration_lock;

int lock_client_data()
{
    return pthread_mutex_lock(&registration_lock);
};

void unlock_client_data()
{
    pthread_mutex_unlock(&registration_lock);
};

void registration_init(void)
{
    nb_registered_clients=0;
    pthread_mutex_init(&registration_lock, NULL);
    bzero(registration_table, MAX_CLIENT * sizeof(client_data_t*));
 
}

client_data_t* registration_lookup(unsigned long key)
{
    int i=0;
    client_data_t* result = NULL;
    for(i=0; i< nb_registered_clients; i++){
        if(registration_table[i]->key == key){
            result = registration_table[i];
            break;
        }
    }
    return result; 
}

int registration_insert(client_data_t* cl)
{    
    if(nb_registered_clients == MAX_CLIENT){
        return -1;
    }
    /* lookup to find if key already exists*/
    client_data_t* lp= registration_lookup(cl->key);
    if(lp != NULL){
        fprintf(stderr, "Error -- id % ld already in use\n", cl->key);
        return -1;
    }

    /* insert cl */
    registration_table[nb_registered_clients]=cl;
    nb_registered_clients++;
    
    return 0;
}


client_data_t* registration_remove(unsigned long key)
{
    int i=0;
    for(i=0; i<nb_registered_clients; i++){
        if(registration_table[i]->key == key){
            break;
        }
    }
    
    if(i == nb_registered_clients){
        fprintf(stderr, "Error -- no client found\n");
        return NULL;
    }
    
    client_data_t* cl= registration_table[i];

    nb_registered_clients--;
    registration_table[i] = registration_table[nb_registered_clients];
    return cl;
}
