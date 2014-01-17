/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

#include "vim.h"

#ifdef FEAT_MESSAGEQUEUE
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

typedef enum { UserInput, DeferredCall } MessageType;

typedef struct message_T
{ 
    struct message_T * next;
    MessageType type;
    void *data;
} message_T;

typedef struct message_queue_T
{
    pthread_mutex_t	mutex;
    pthread_cond_t	cond;
    message_T		*head;
    message_T		*tail;
} message_queue_T;

message_queue_T	    message_queue;

pthread_t	    char_wait_thread;
pthread_mutex_t	    char_wait_mutex;
pthread_cond_t	    char_wait_cond;

pthread_mutex_t	    sleep_mutex;
pthread_cond_t	    sleep_cond;

/* global lock that must be held by any thread that will call or return
 * control to vim */ 
pthread_mutex_t     io_mutex;

int		    queue_initialized = FALSE;

/* Current values for ui_inchar */
char_u		    *cur_buf;
int		    cur_maxlen;
long		    cur_wtime;
int		    cur_tb_change_cnt;
int		    cur_len;

    static void
pthread_error(const char *msg)
{
    fprintf(stderr, "\n%s\n", msg);
    mch_exit(EXIT_FAILURE);
}


/*
 * Private helpers to used to deal with pthread data
 */
    static void
lock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_lock(mutex) != 0)
	pthread_error("Error acquiring lock");
}


    static void
unlock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_unlock(mutex) != 0)
	pthread_error("Error releasing lock");
}


    static void
cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    if (pthread_cond_wait(cond, mutex) != 0)
	pthread_error("Error waiting condition");
}


    static int
timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, long ms)
{
    struct timespec ts;
    int		    result;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000;

    result = pthread_cond_timedwait(cond, mutex, &ts);

    switch (result)
    {
	case ETIMEDOUT:
	    return FALSE;
	case EINVAL:
	    pthread_error("Value specified by abstime is invalid");
	case EPERM:
	    pthread_error("Doesn't own the mutex");
	default:
	    pthread_error("Unknown error waiting condition");
    }

    return TRUE;
}


    static void
pthread_sleep(long ms)
{
    timedwait(&sleep_cond, &sleep_mutex, ms);
}


    static void
cond_notify(pthread_cond_t *cond)
{
    if (pthread_cond_signal(cond) != 0)
	pthread_error("Error signaling condition");
}


/* 
 * Insert a message at the end of the queue.
 */
    static void
queue_push(type, data)
    MessageType	    type;    /* Type of message */
    void	    *data;   /* Data associated with the message */
{
    int empty;
    message_T *msg = (message_T *)alloc(sizeof(message_T));
    msg->type = type;
    msg->data = data;
    msg->next = NULL;

    /* Acquire queue lock */
    lock(&message_queue.mutex);

    empty = message_queue.head == NULL;

    if (empty) {
	/* Put the message at the beginning for immediate consumption */
	msg->next = message_queue.head;
	message_queue.head = msg;

	/*
	 * Queue was empty and consequently the main thread was waiting,
	 * so wake it up to continue after the lock is released
	 */
	if (empty)
	    cond_notify(&message_queue.cond);

    } else {
	/* 
	 * There are pending messages, put this one at the end, adjusting the
	 * next pointer.
	 */
	if (message_queue.tail == NULL) {
	    message_queue.head->next = msg;
	} else {
	    message_queue.tail->next = msg;
	}
	message_queue.tail = msg;
    }

    unlock(&message_queue.mutex);
}


/* Take a message from the beginning of the queue */
    static message_T *
queue_shift(long ms)
{
    int		wait_result = TRUE;
    message_T	*rv = NULL;

    lock(&message_queue.mutex);

    if (message_queue.head == NULL) {
	/* Queue is empty, cond_wait for more messages */
	if (ms >= 0)
	    wait_result = timedwait(&message_queue.cond, &message_queue.mutex, ms);
	else
	    cond_wait(&message_queue.cond, &message_queue.mutex);
    }

    if (wait_result)
    {
	rv = message_queue.head;
	message_queue.head = rv->next;
    }

    unlock(&message_queue.mutex);

    return rv;
}

/* Listen for user input in a separate thread, but only
 * when asked by the main thead
 */
    static void *
vgetcs(arg)
    void	*arg UNUSED; /* Unsused thread start argument */
{
    while (TRUE)
    {
	/* Since this function potentially modifies state(eg: update screen)
	 * we need to synchronize io with the main thread.
	 *
	 * This means we must use a timeout to periodically unlock the
	 * io mutex so the main thread can continue to process other
	 * events */
	while (TRUE)
	{
	    lock(&io_mutex);
	    cur_len = ui_inchar(cur_buf, cur_maxlen, 100, cur_tb_change_cnt);
	    if (cur_len > 0)
		break;
	    unlock(&io_mutex);
	}
	queue_push(UserInput, NULL);
	lock(&char_wait_mutex);
	unlock(&io_mutex);
	cond_wait(&char_wait_cond, &char_wait_mutex);
	unlock(&char_wait_mutex);
    }
}


/*
 * Initialize the message queue and start listening for user input in a
 * separate thread.
 */
    static void
queue_init()
{
    pthread_attr_t attr;
    pthread_condattr_t condattr;

    if (pthread_condattr_init(&condattr) != 0)
	pthread_error("Failed to init condattr");

    if (pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC) != 0)
	pthread_error("Failed to init condattr");

    if (pthread_mutex_init(&char_wait_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&char_wait_cond, &condattr) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&sleep_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&sleep_cond, &condattr) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&message_queue.mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&message_queue.cond, &condattr) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&io_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    /* This will be held by the main thread most of the time */
    lock(&io_mutex);

    message_queue.head = NULL;
    message_queue.tail = NULL;

    if (pthread_attr_init(&attr) != 0)
	pthread_error("Failed to initialize the thread attribute");

    if (pthread_create(&char_wait_thread, &attr, &vgetcs, NULL) != 0)
	pthread_error("Failed to initialize the user input thread");
}

    int
ev_inchar(buf, maxlen, wtime, tb_change_cnt)
    char_u	*buf;
    int		maxlen;
    long	wtime;
    int		tb_change_cnt;
{
    message_T	*msg;
    MessageType type;
    void	*data;

    if (!queue_initialized)
    {
	queue_init();
	queue_initialized = TRUE;
    }

    if (wtime == 0) /* This would not block, so just call it directly */
	return ui_inchar(buf, maxlen, wtime, tb_change_cnt);

    cur_buf = buf;
    cur_maxlen = maxlen;
    cur_tb_change_cnt = tb_change_cnt;

    unlock(&io_mutex);
    msg = queue_shift(wtime);
    lock(&io_mutex); /* Lock io since we are returning to vim */
    lock(&char_wait_mutex);
    cond_notify(&char_wait_cond);
    unlock(&char_wait_mutex);

    if (!msg) return 0;

    type = msg->type;
    data = msg->data;
    vim_free(msg);

    if (type == UserInput)
	return cur_len;

    return 0;
}


    void
message_loop_call(char * func)
{
    queue_push(DeferredCall, strdup(func));
}

#endif
