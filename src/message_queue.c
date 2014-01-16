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
int		    is_polling = FALSE;
int		    is_waiting = FALSE;

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
wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    if (pthread_cond_wait(cond, mutex) != 0)
	pthread_error("Error waiting condition");
}


    static int
timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, long ms)
{
    struct timespec ts;
    struct timeval  tv;
    int		    result;

    gettimeofday(&tv, NULL);

    ts.tv_sec = tv.tv_sec + (ms / 1000);
    ts.tv_nsec = (1000 * tv.tv_usec) + ((ms % 1000) * 1000000);

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
notify(pthread_cond_t *cond)
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
	    notify(&message_queue.cond);

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
	/* Queue is empty, wait for more messages */
	if (ms >= 0)
	    wait_result = timedwait(&message_queue.cond, &message_queue.mutex, ms);
	else
	    wait(&message_queue.cond, &message_queue.mutex);
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
    int avail;

    while (TRUE)
    {
	avail = FALSE;
	lock(&char_wait_mutex);
	is_waiting = TRUE;

	/* Ready to be notified */
	wait(&char_wait_cond, &char_wait_mutex);

	is_waiting = FALSE;
	is_polling = TRUE;

	unlock(&char_wait_mutex);

	/* Since this function potentially modifies state(eg: update screen)
	 * we need to synchronize with the main thread.
	 *
	 * This means we must use a timeout to periodically unlock the
	 * io mutex */
	while (!avail)
	{
	    lock(&io_mutex);
	    cur_len = ui_inchar(cur_buf, cur_maxlen, 100, cur_tb_change_cnt);
	    if (cur_len > 0)
	    {
		is_polling = FALSE;
		avail = TRUE;
		queue_push(UserInput, NULL);
	    }
	    unlock(&io_mutex);
	}

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

    if (pthread_mutex_init(&char_wait_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&char_wait_cond, NULL) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&sleep_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&sleep_cond, NULL) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&message_queue.mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&message_queue.cond, NULL) != 0)
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


/* Notify the background thread that it should wait for a character and
 * then send a message */
    static void
char_wait()
{
    if (is_polling) {
	unlock(&io_mutex);
	return;
    }
    unlock(&io_mutex);

    /* The following wait will ensure we dont notify without the background
     * thread in wait state, which would result in a deadlock */
    while (!is_waiting) pthread_sleep(100);

    lock(&char_wait_mutex);
    notify(&char_wait_cond);
    unlock(&char_wait_mutex);
}


    int
msg_inchar(buf, maxlen, wtime, tb_change_cnt)
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

    /* Notify the background thread that it should wait for a
     * character */
    char_wait();

    while (TRUE)
    {
	/* 
	 * Wait for a message, which can be an 'UserInput' message
	 * set by the background thread or a 'DeferredCall' message
	 * indirectly set by vimscript.
	 */
	msg = queue_shift(wtime);
	lock(&io_mutex); /* Lock io since we are returning control */

	if (!msg) return 0;

	type = msg->type;
	data = msg->data;
	vim_free(msg);

	switch (type)
	{
	    case UserInput:
		return cur_len;
	    case DeferredCall:
		break;
	}
    }

    return 0;
}


    void
message_loop_call(char * func)
{
    queue_push(DeferredCall, strdup(func));
}

#endif
