/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

/* This file implements vim event loop. Here's how it works:
 *
 * The function 'ev_trigger' will push events to a thread-safe queue(ev_trigger
 * may be called by any thread).
 *
 * The function 'ev_next' provides a drop-in replacement for 'ui_inchar' which
 * is always called when new characters are needed.
 *
 * The difference is that when an infinite timeout is passed as argument(-1),
 * ev_next will poll the queue for new events at regular intervals. When a new
 * event is available, it will pull from the queue(in a safe way) and return the
 * K_USEREVENT special key, which will be handled at higher levels
 * by triggering 'User' autocommands for that event.
 *
 * This event loop was 'injected' in vim by replacing all calls to ui_inchar by
 * the io_inchar macro, which is translated into ev_next when compiled with
 * --enable-eventloop or ui_inchar otherwise. Since vim has specialized loops
 *  for receiving keys in each mode, the autocommand invocation has to be
 *  handled in each loop.
 * */
#include "vim.h"

#ifdef FEAT_EVENT_LOOP
#include <pthread.h>

#define POLL_INTERVAL 100 /* Interval used to poll for events */

/* An event has a name that will be matched against the autocommand pattern (au
 * User [PATTERN]) and an argument which will be set to v:cmdarg before the
 * autocommand is called */
typedef struct ev_T
{ 
    struct ev_T * next;
    char_u *name, *event_args;
} ev_T;

/* The event queue, which is implemented as a singly-linked list protected by
 * a mutex */
typedef struct event_queue_T
{
    pthread_mutex_t	mutex;
    /* The first element of the queue, or NULL if the queue is empty. */
    ev_T		*head;
    /* The last element of the queue, or NULL if the queue is empty. */
    ev_T		*tail;
} event_queue_T;

static event_queue_T	    event_queue;

static pthread_once_t once_control = PTHREAD_ONCE_INIT;

/*
 * Private helpers to used to deal with threading structures
 */
    static void
pthread_error(const char *msg)
{
    fprintf(stderr, "\n%s\n", msg);
    mch_exit(EXIT_FAILURE);
}


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
init_queue()
{
    if (pthread_mutex_init(&event_queue.mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");
}


/* 
 * TODO This should block when vim is unable to process events
 * Insert a event at the end of the queue.
 */
    static void
queue_push(name, event_args)
    char_u              *name;		/* Event name */
    char_u              *event_args;	/* Event arg */
{
    ev_T *ev = (ev_T *)alloc(sizeof(ev_T));
    ev->name = name;
    ev->event_args = event_args;
    ev->next = NULL;

    pthread_once(&once_control, init_queue);

    /* Nothing much to comment here, basic linked list insertion protected
     * by the queue mutext */
    lock(&event_queue.mutex);

    if (event_queue.head == NULL) {
	event_queue.head = ev;
    }

    if (event_queue.tail == NULL) {
	event_queue.tail = ev;
    } else {
	event_queue.tail->next = ev;
    }

    unlock(&event_queue.mutex);
}


/* Take an event from the beginning of the queue.
 * Returns NULL if the queue is empty. */
    static ev_T *
queue_shift()
{
    ev_T	*rv = NULL;
    ev_T	*next = NULL;

    pthread_once(&once_control, init_queue);

    lock(&event_queue.mutex);
    rv = event_queue.head;
    if (rv != NULL) {
        next = rv->next;
        /* Prevent unserialized access to subsequent elements */
        rv->next = NULL;
        event_queue.head = next;
        if (next == NULL) {
            /* We reached the end of the queue */
            event_queue.tail = NULL;
        }
    }
    unlock(&event_queue.mutex);

    return rv;
}


/* Returns 1 if the queue is non-empty, 0 otherwise */
    static int
queue_peek()
{
    int		rv = 0;

    pthread_once(&once_control, init_queue);

    lock(&event_queue.mutex);
    if (event_queue.head != NULL)
        rv = 1;
    unlock(&event_queue.mutex);

    return rv;
}


/* Set the USEREVENT special key into the input buffer */
    static int
event_user(buf)
    char_u	*buf;
{
    buf[0] = K_SPECIAL;
    buf[1] = KS_EXTRA;
    buf[2] = (int)KE_USEREVENT;

    return 3;
}


/* Set the CURSORHOLD special key into the input buffer */
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
 * Bridge between vim and the event loop, 'disguised' as a function that
 * returns keys(where one of the special keys is K_USEREVENT
 */
    int
ev_next(buf, maxlen, wtime, tb_change_cnt)
    char_u	*buf;
    int		maxlen;
    long	wtime;
    int		tb_change_cnt;
{
    int		len;
    int		trig_curshold;
    long	ellapsed;

    /* Dont poll for events when a timeout is passed */
    if (wtime >= 0)
	return ui_inchar(buf, maxlen, wtime, tb_change_cnt);

    trig_curshold = trigger_cursorhold();

    if (trig_curshold)
	ellapsed = 0;	    /* Time waiting for a char in milliseconds */
    else
	before_blocking();  /* Normally called when doing a blocking wait */

    while (!queue_peek())
    {
	len = ui_inchar(buf, maxlen, POLL_INTERVAL, tb_change_cnt);
	ellapsed += POLL_INTERVAL;

	if (len > 0)
	    return len; /* Got something, return now */

	/* We must trigger cursorhold events ourselves. Normally cursorholds
	 * are triggered at a platform-specific lower function when an
	 * infinite timeout is passed, but those won't get the chance
	 * because we never pass infinite timeout in order to poll for
	 * events from other threads */
	if (trig_curshold && ellapsed >= p_ut)
	    return event_cursorhold(buf);

    }

    return event_user(buf);
}

/* Push an event to the queue. This is the function other threads will call
 * when they need to notify vimscript of something*/
    void
ev_trigger(char_u *name, char_u *event_args)
{
    queue_push(name, event_args);
}


/*
 * Invoke the User autocommand. Called by other layers after we return
 * K_USEREVENT.
 */
    void
apply_event_autocmd()
{
    ev_T	*e = NULL;

    while ((e = queue_shift()) != NULL)
    {
        if (e->event_args != NULL)
            set_vim_var_string(VV_EVENT_ARG, e->event_args, -1);
        else
            set_vim_var_string(VV_EVENT_ARG, (char_u *)"", -1);

        apply_autocmds(EVENT_USER, e->name, NULL, TRUE, NULL);

        vim_free(e->name);
        vim_free(e->event_args);
        vim_free(e);
    }
}

#endif
