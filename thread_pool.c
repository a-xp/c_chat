#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "thread_pool.h"


static void await_flag(await_flag_t* flag)
{
	pthread_mutex_lock(&flag->mx);
	while (flag->is_set!=1) {
		pthread_cond_wait(&flag->con, &flag->mx);
	}
	flag->is_set=0;
	pthread_mutex_unlock(&flag->mx);    
}
static void notify_flag(await_flag_t* flag)
{
	pthread_mutex_lock(&flag->mx);
	flag->is_set = 1;
	pthread_cond_signal(&flag->con);
	pthread_mutex_unlock(&flag->mx);    
}
static void init_await_flag(await_flag_t* flag)
{
    pthread_mutex_init(&(flag->mx), NULL);
	pthread_cond_init(&(flag->con), NULL);
    flag->is_set = 0;
}

static void worker_run(thread_pool_t* pool_ptr)
{
    task_t* task_ptr;
    void (*task_cb)(void* param);
    void* param;
    
    while(1){
        await_flag(&(pool_ptr->task_rdy_flag));
        pthread_mutex_lock(&(pool_ptr->task_queue_lock));
        task_ptr = pool_ptr->top;
        switch(pool_ptr->len){            
            case 0:  break;            
            case 1: pool_ptr->top = NULL;
                    pool_ptr->bot = NULL;
                    pool_ptr->len = 0;
                    break;
            default: pool_ptr->top = task_ptr->prev;
                     pool_ptr->len--;
                     notify_flag(&(pool_ptr->task_rdy_flag));
                        
        }
        pthread_mutex_unlock(&(pool_ptr->task_queue_lock));
        
        if (task_ptr) {
            task_cb = task_ptr->callback;
            param  = task_ptr->param;
            task_cb(param);
        }
    }
}

thread_pool_t* thread_pool_create(int num){

	if (num < 0){
		num = 1;
	}
	thread_pool_t* pool_ptr;
	pool_ptr = (thread_pool_t*)malloc(sizeof(thread_pool_t));
    pool_ptr->len = 0;
	pool_ptr->top = NULL;
    pool_ptr->bot = NULL;
    pthread_mutex_init(&(pool_ptr->task_queue_lock), NULL);
	init_await_flag(&pool_ptr->task_rdy_flag);
	int n;
    pthread_t thread_ptr;
	for (n=0; n<num; n++){
        pthread_create(&thread_ptr, NULL, (void *)worker_run, pool_ptr);
        pthread_detach(thread_ptr);
    }

	return pool_ptr;
}

void thread_pool_submit(thread_pool_t* pool_ptr, void (*task)(void*), void* param)
{
	task_t* task_ptr;
	task_ptr=(task_t*)malloc(sizeof(task_t));
	task_ptr->callback=task;
	task_ptr->param=param;
    task_ptr->prev = NULL;

    pthread_mutex_lock(&(pool_ptr->task_queue_lock));
	if(pool_ptr->len){
        pool_ptr->bot->prev = task_ptr;
        pool_ptr->bot = task_ptr;
    }else{
        pool_ptr->top = task_ptr;
        pool_ptr->bot = task_ptr;					
	}
	pool_ptr->len++;
	notify_flag(&(pool_ptr->task_rdy_flag));
    pthread_mutex_unlock(&(pool_ptr->task_queue_lock));   
};



