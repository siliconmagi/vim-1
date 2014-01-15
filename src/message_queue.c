#include <unistd.h>
#include "vim.h"

#ifdef FEAT_MESSAGEQUEUE

#include <pthread.h>

#include "message_queue.h"

typedef struct message_queue_T
{
    pthread_mutex_t	mutex;
    pthread_cond_t	cond;
    message_T		*head;
    message_T		*tail;
} message_queue_T;

message_queue_T	    message_queue;

pthread_t	    input_thread;
pthread_mutex_t	    input_mutex;
pthread_cond_t	    input_cond;

int		    waiting_for_input;
pthread_mutex_t	    waiting_for_input_mutex;

pthread_mutex_t     io_mutex;

int		    queue_initialized = FALSE;

/* 
 * FIXME: Figure out the right way to deal with such errors by asking
 * on the list
 */
    void
pthread_error(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    mch_exit(EXIT_FAILURE);
}

/*
 * Private helpers to used to lock/unlock the input mutex
 */
    static void
lock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_lock(mutex) != 0)
	pthread_error("Error acquiring user input lock");
}

    static void
unlock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_unlock(mutex) != 0)
	pthread_error("Error releasing user input lock");
}

/*
 * Function used by the background thread to wait for a signal to read
 * input from the main thread
 */
    static void
input_wait()
{
    lock(&input_mutex);

    if (pthread_cond_wait(&input_cond, &input_mutex) != 0)
	pthread_error("Failed to wait for condition");
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
	lock(&waiting_for_input_mutex);
	waiting_for_input = FALSE;
	unlock(&waiting_for_input_mutex);

	// Only try to read input when asked by the main thread
	input_wait();

	// Dont let the main thread call 'input_notify' or else it would block
	lock(&waiting_for_input_mutex);
	waiting_for_input = TRUE;
	unlock(&waiting_for_input_mutex);

	while (TRUE) {
	    input_acquire();
	    if (char_avail()) {
		input_release();
		break;
	    }
	    input_release();
	    usleep(50000);
	}

	unlock(&input_mutex);
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

    if (pthread_mutex_init(&input_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&input_cond, NULL) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&message_queue.mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_cond_init(&message_queue.cond, NULL) != 0)
	pthread_error("Failed to init the condition");

    if (pthread_mutex_init(&waiting_for_input_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    if (pthread_mutex_init(&io_mutex, NULL) != 0)
	pthread_error("Failed to init the mutex");

    message_queue.head = NULL;
    message_queue.tail = NULL;

    if (pthread_attr_init(&attr) != 0)
	pthread_error("Failed to initialize the thread attribute");

    if (pthread_create(&input_thread, &attr, &vgetcs, NULL) != 0)
	pthread_error("Failed to initialize the user input thread");
}
/*
 * Function used by the main thread to notify that it should read something
 */
    void
input_notify()
{
    lock(&waiting_for_input_mutex);
    if (waiting_for_input) {
	unlock(&waiting_for_input_mutex);
	return;
    }
    unlock(&waiting_for_input_mutex);


    lock(&input_mutex);

    if (pthread_cond_broadcast(&input_cond) != 0)
	pthread_error("Failed to acquire lock");

    unlock(&input_mutex);
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
	if (empty && pthread_cond_broadcast(&message_queue.cond) != 0)
	    pthread_error("Failed to wake the main thread");

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
	/* 
	 * Queue is empty, temporarily release the lock and wait for
	 * more messages
	 */
	if (pthread_cond_wait(&message_queue.cond,
		    &message_queue.mutex) != 0)
	    pthread_error("Failed to wait for condition");
    }

    rv = message_queue.head;
    message_queue.head = rv->next;

    unlock(&message_queue.mutex);

    return rv;
}


    void
input_acquire()
{
    lock(&io_mutex);
}


    void
input_release()
{
    unlock(&io_mutex);
}


    void
queue_ensure()
{
    if (queue_initialized)
	return;

    queue_init();
    queue_initialized = TRUE;
}

#endif
