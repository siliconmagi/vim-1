#include <unistd.h>
#include <pthread.h>
#include "vim.h"

#ifdef FEAT_MESSAGEQUEUE


#include "message_queue.h"

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

pthread_mutex_t     io_mutex;

int		    queue_initialized = FALSE;
int		    is_polling = FALSE;
int		    is_waiting = FALSE;

    void
pthread_error(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
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

    static void
notify(pthread_cond_t *cond)
{
    if (pthread_cond_signal(cond) != 0)
	pthread_error("Error signaling condition");
}

/*
 * This function will listen for user input in a separate thread, but only
 * when asked by the main thead
 */
    static void *
vgetcs(arg)
    void	*arg UNUSED; /* Unsused thread start argument */
{
    while (TRUE)
    {
	lock(&char_wait_mutex);
	is_waiting = TRUE;

	/* Ready to be notified */
	wait(&char_wait_cond, &char_wait_mutex);

	is_waiting = FALSE;
	is_polling = TRUE;

	unlock(&char_wait_mutex);

	while (TRUE) {
	    io_lock();
	    if (char_avail()) {
		is_polling = FALSE;
		io_unlock();
		break;
	    }
	    io_unlock();
	    usleep(50000);
	}

	queue_push(UserInput, NULL);
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

    if (pthread_mutex_init(&message_queue.mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&message_queue.cond, NULL) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&io_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    message_queue.head = NULL;
    message_queue.tail = NULL;

    if (pthread_attr_init(&attr) != 0)
	pthread_error("Failed to initialize the thread attribute");

    if (pthread_create(&char_wait_thread, &attr, &vgetcs, NULL) != 0)
	pthread_error("Failed to initialize the user input thread");
}
/*
 * Function used by the main loop to notify the background thread that it
 * should wait for a character
 */
    void
char_wait()
{
    io_lock();
    if (is_polling) {
	io_unlock();
	return;
    }
    io_unlock();

    /* The following wait will ensure we dont notify without the background
     * thread in wait state, which would result in a deadlock */
    while (!is_waiting) usleep(50000);

    lock(&char_wait_mutex);
    notify(&char_wait_cond);
    unlock(&char_wait_mutex);
}

/* 
 * Insert a message at the end of the queue.
 */
    void
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
    message_T *
queue_shift()
{
    message_T *rv;

    lock(&message_queue.mutex);

    if (message_queue.head == NULL) {
	/* Queue is empty, wait for more messages */
	wait(&message_queue.cond, &message_queue.mutex);
    }

    rv = message_queue.head;
    message_queue.head = rv->next;

    unlock(&message_queue.mutex);

    return rv;
}


    void
io_lock()
{
    lock(&io_mutex);
}


    void
io_unlock()
{
    unlock(&io_mutex);
}


    void
queue_ensure()
{
    if (queue_initialized) return;

    queue_init();
    queue_initialized = TRUE;
}

#endif
