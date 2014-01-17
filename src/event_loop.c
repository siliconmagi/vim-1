/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

#include "vim.h"

#ifdef FEAT_EVENT_LOOP
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

typedef enum { Input, Custom } EventType;

typedef struct ev_T
{ 
    struct ev_T * next;
    EventType type;
    struct event_data {
	char_u *event, *event_args;
    } *data;
} ev_T;

typedef struct event_queue_T
{
    pthread_mutex_t	mutex;
    pthread_cond_t	cond;
    ev_T		*head;
    ev_T		*tail;
} event_queue_T;

event_queue_T	    event_queue;

pthread_t	    background_thread;

pthread_mutex_t	    semaph_mutex;
pthread_cond_t	    semaph_cond;

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


#define SECOND 1000000000
#define MILLISECOND 1000000

    static int
timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, long ms)
{
    struct  timespec ts;
    int	    result;
    long    extra_secs = ms / 1000, extra_nsecs = (ms % 1000) * MILLISECOND;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    ts.tv_sec += extra_secs + (
	    extra_nsecs > SECOND ? (extra_nsecs / SECOND) : 0);
    ts.tv_nsec += extra_nsecs % SECOND;

    result = pthread_cond_timedwait(cond, mutex, &ts);

    switch (result)
    {
	case ETIMEDOUT:
	    return FALSE;
	case EINVAL:
	    pthread_error("Value specified by abstime is invalid");
	case EPERM:
	    pthread_error("Doesn't own the mutex");
    }

    if (result == 0)
	return TRUE;

    pthread_error("Unknown error waiting condition");
}


    static void
cond_notify(pthread_cond_t *cond)
{
    if (pthread_cond_signal(cond) != 0)
	pthread_error("Error signaling condition");
}


/* 
 * Insert a event at the end of the queue.
 */
    static void
queue_push(type, data)
    EventType		type;    /* Event type */
    struct event_data	*data;   /* Event data */
{
    int empty;
    ev_T *ev = (ev_T *)alloc(sizeof(ev_T));
    ev->type = type;
    ev->data = data;
    ev->next = NULL;

    /* Acquire queue lock */
    lock(&event_queue.mutex);

    empty = event_queue.head == NULL;

    if (empty) {
	/* Put the event at the beginning for immediate consumption */
	ev->next = event_queue.head;
	event_queue.head = ev;

	/*
	 * Queue was empty and consequently the main thread was waiting,
	 * so wake it up to continue after the lock is released
	 */
	if (empty)
	    cond_notify(&event_queue.cond);

    } else {
	/* 
	 * There are pending events, put this one at the end, adjusting the
	 * next pointer.
	 */
	if (event_queue.tail == NULL) {
	    event_queue.head->next = ev;
	} else {
	    event_queue.tail->next = ev;
	}
	event_queue.tail = ev;
    }

    unlock(&event_queue.mutex);
}


/* Take an event from the beginning of the queue */
    static ev_T *
queue_shift(long ms)
{
    int		wait_result = TRUE;
    ev_T	*rv = NULL;

    lock(&event_queue.mutex);

    if (event_queue.head == NULL) {
	/* Queue is empty, wait for more */
	if (ms >= 0)
	    wait_result = timedwait(&event_queue.cond, &event_queue.mutex, ms);
	else
	    cond_wait(&event_queue.cond, &event_queue.mutex);
    }

    if (wait_result)
    {
	rv = event_queue.head;
	event_queue.head = rv->next;
    }

    unlock(&event_queue.mutex);

    return rv;
}

/* Listen for user input in a separate thread, periodically releasing
 * the io_mutex so the main thread can process events
 */
    static void *
inchar_loop(arg)
    void	*arg UNUSED; /* Unsused thread start argument */
{
    long wt;

    while (TRUE)
    {
	/* Since this function potentially modifies state(eg: update screen)
	 * we need to synchronize its access with the main thread.
	 *
	 * This means we must use a timeout to periodically unlock the
	 * io mutex so the main thread can continue to process other
	 * events */
	while (TRUE)
	{
	    /* Wait for at most 100 ms */
	    lock(&io_mutex);
	    wt = cur_wtime >= 100 ? 100 : cur_wtime;
	    cur_len = ui_inchar(cur_buf, cur_maxlen, wt, cur_tb_change_cnt);
	    if (cur_len > 0)
		break;
	    unlock(&io_mutex);
	}
	queue_push(Input, NULL);
	lock(&semaph_mutex);
	unlock(&io_mutex);
	cond_wait(&semaph_cond, &semaph_mutex);
	unlock(&semaph_mutex);
    }
}


/*
 * Initialize the event queue and start listening for user input in a
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

    if (pthread_mutex_init(&semaph_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&semaph_cond, &condattr) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&event_queue.mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&event_queue.cond, &condattr) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&io_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    /* This will be held by the main thread most of the time */
    lock(&io_mutex);

    event_queue.head = NULL;
    event_queue.tail = NULL;

    if (pthread_attr_init(&attr) != 0)
	pthread_error("Failed to initialize the thread attribute");

    if (pthread_create(&background_thread, &attr, &inchar_loop, NULL) != 0)
	pthread_error("Failed to initialize the user input thread");
}

    int
ev_next(buf, maxlen, wtime, tb_change_cnt)
    char_u	*buf;
    int		maxlen;
    long	wtime;
    int		tb_change_cnt;
{
    ev_T	*ev;
    EventType	type;
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
    cur_wtime = wtime;
    cur_tb_change_cnt = tb_change_cnt;

    unlock(&io_mutex);
    ev = queue_shift(wtime);
    lock(&io_mutex); /* Lock io since we are returning to vim */
    lock(&semaph_mutex);
    cond_notify(&semaph_cond);
    unlock(&semaph_mutex);

    if (!ev) return 0;

    type = ev->type;
    data = ev->data;
    vim_free(ev);

    if (type == Input)
	return cur_len;

    return 0;
}


    void
ev_emit(char * event, char * arg)
{
    queue_push(Custom, strdup(event));
}

#endif
