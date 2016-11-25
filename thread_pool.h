#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__

#include <pthread.h>

typedef struct await_flag {
	char is_set;
	pthread_mutex_t mx;
	pthread_cond_t con;
} await_flag_t;

typedef struct task{
	struct task*  prev;                   
	void   (*callback)(void* param);     
	void*  param;                         
} task_t;

typedef struct thread_pool{                
	await_flag_t task_rdy_flag;
    pthread_mutex_t task_queue_lock;    
	task_t* top;                         
	task_t* bot;                          
	int   len;       
} thread_pool_t;

thread_pool_t* thread_pool_create(int num);

void thread_pool_submit(thread_pool_t* pool, void (*task)(void*), void* param);

#endif