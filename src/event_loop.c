/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

#include "vim.h"

#ifdef FEAT_EVENT_LOOP
#include <pthread.h>

#define INTERRUPT_INTERVAL 100 /* Interval used to check for events */

typedef struct ev_T
{ 
    struct ev_T * next;
    char_u *event, *event_args;
} ev_T;

typedef struct event_queue_T
{
    pthread_mutex_t	mutex;
    ev_T		*head;
    ev_T		*tail;
} event_queue_T;

event_queue_T	    event_queue;

int		    queue_initialized = FALSE;

char_u *current_event;
char_u *current_event_args;

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


/* 
 * Insert a event at the end of the queue.
 */
    static void
queue_push(event, event_args)
    char_u              *event;		/* Event type */
    char_u              *event_args;	/* Event type */
{
    ev_T *ev = (ev_T *)alloc(sizeof(ev_T));
    ev->event = event;
    ev->event_args = event_args;
    ev->next = NULL;

    lock(&event_queue.mutex);

    if (event_queue.head == NULL) {
	ev->next = event_queue.head;
	event_queue.head = ev;
    } else {

	if (event_queue.tail == NULL)
	    event_queue.head->next = ev;
	else
	    event_queue.tail->next = ev;

	event_queue.tail = ev;
    }

    unlock(&event_queue.mutex);
}


/* Take an event from the beginning of the queue */
    static ev_T *
queue_shift()
{
    ev_T	*rv = NULL;

    lock(&event_queue.mutex);
    rv = event_queue.head;
    event_queue.head = rv->next;
    unlock(&event_queue.mutex);

    return rv;
}


    static int
event_user(buf)
    char_u	*buf;
{
    buf[0] = K_SPECIAL;
    buf[1] = KS_EXTRA;
    buf[2] = (int)KE_USEREVENT;

    return 3;
}


    static int
event_cursorhold(buf)
    char_u	*buf;
{
    buf[0] = K_SPECIAL;
    buf[1] = KS_EXTRA;
    buf[2] = (int)KE_CURSORHOLD;

    return 3;
}


/*
 * Initialize the event queue
 */
    static void
queue_init()
{
    if (pthread_mutex_init(&event_queue.mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    event_queue.head = NULL;
    event_queue.tail = NULL;
}


    int
ev_next(buf, maxlen, wtime, tb_change_cnt)
    char_u	*buf;
    int		maxlen;
    long	wtime;
    int		tb_change_cnt;
{
    ev_T	*ev;
    int		len;
    long	ellapsed = 0;

    if (!queue_initialized)
    {
	queue_init();
	queue_initialized = TRUE;
    }

    if (wtime >= 0) /* Dont poll for events when waiting for more keys */
	return ui_inchar(buf, maxlen, wtime, tb_change_cnt);

    do
    {
	len = ui_inchar(buf, maxlen, 100, tb_change_cnt);
	ellapsed += 100;

	if (len > 0)
	    return len;

	/* We must trigger cursorhold events ourselves since its normally done
	 * at lower levels which will never get the chance due to never
	 * receiving negative timeout(-1) */
	if (ellapsed >= p_ut)
	    return event_cursorhold(buf);

    } while (event_queue.head == NULL);

    ev = queue_shift();
    current_event = ev->event;
    current_event_args = ev->event_args;
    vim_free(ev);

    return event_user(buf);
}


    void
ev_trigger(char_u *event, char_u *event_args)
{
    queue_push(event, event_args);
}

/*
 * Trigger User event
 */
    void
apply_event_autocmd()
{
    apply_autocmds(EVENT_USER, current_event, NULL, TRUE, NULL);
    vim_free(current_event);
    vim_free(current_event_args);
}

#endif
