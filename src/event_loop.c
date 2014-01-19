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
    ev_T		*head;
    ev_T		*tail;
} event_queue_T;

pthread_mutex_t	    vim_mutex;
event_queue_T	    event_queue;

/* Flag check if the queue is initialized */
int		    queue_initialized = FALSE;

/* Module-globals that contain the event name/arg currently being processed */
char_u *current_event;
char_u *current_event_args;

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

    /* Nothing much to comment here, basic linked list insertion protected
     * by the queue mutext */
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
    ev_T	*ev;
    int		len;
    int		trig_curshold;
    long	ellapsed;

    /* Initialize the queue mutex if not done already */
    if (!queue_initialized)
    {
	if (pthread_mutex_init(&event_queue.mutex, NULL) != 0)
	    pthread_error("Failed to init the mutex");

	if (pthread_mutex_init(&vim_mutex, NULL) != 0)
	    pthread_error("Failed to init the mutex");

	queue_initialized = TRUE;
    }

    /* Dont poll for events when a timeout is passed */
    if (wtime >= 0)
	return ui_inchar(buf, maxlen, wtime, tb_change_cnt);

    trig_curshold = trigger_cursorhold();

    if (trig_curshold)
	ellapsed = 0;	    /* Time waiting for a char in milliseconds */
    else
	before_blocking();  /* Normally called when doing a blocking wait */

    do
    {
	len = ui_inchar(buf, maxlen, POLL_INTERVAL, tb_change_cnt);

	if (trig_curshold)
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

    } while (event_queue.head == NULL);

    /* Got an event, shift from the queue and set the event parameters */
    ev = queue_shift();
    current_event = ev->name;
    current_event_args = ev->event_args;
    vim_free(ev);

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
apply_event_autocmd(void)
{
    if (current_event_args != NULL)
	set_vim_var_string(VV_EVENT_ARG, current_event_args, -1);
    else
	set_vim_var_string(VV_EVENT_ARG, (char_u *)"", -1);

    apply_autocmds(EVENT_USER, current_event, NULL, TRUE, NULL);

    vim_free(current_event);
    vim_free(current_event_args);
}

#endif
