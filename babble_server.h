#ifndef __BABBLE_SERVER_H__
#define __BABBLE_SERVER_H__

#include <stdio.h>

#include "babble_types.h"
#include "thpool.h"

/* used to create a timeline of publications*/
typedef struct timeline_item{
    publication_t *pub;   /* the publication */
    client_data_t *client;   /* publication author */
    struct timeline_item *next;
} timeline_item_t;

/* server starting date */
extern time_t server_start;

/* io threads pools */
extern threadpool conn_workers_pool;
extern threadpool cmd_workers_pool;

/* Init functions*/
void server_data_init(void);
int server_connection_init(int port);
int server_connection_accept(int sock);

/* new object */
command_t* new_command(unsigned long key);

/* Display functions */
void display_command(command_t *cmd, FILE* stream);

/* Error management */
int notify_parse_error(command_t *cmd, char *input);

/* High level comm function */
int write_to_client(unsigned long key, int size, void* buf);

void connection_listener(session_t* sess);
void cmd_executor();

#endif
