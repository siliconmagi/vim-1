/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

/* Job control module for cooperating with child processes in a non-blocking
 * way.
 *
 * Job polling is done by 'job_activity_poll', a drop-in replacement for
 * 'ui_inchar' which is always called when idling for new characters are
 * needed.
 *
 * When some job produces data, the K_JOB_ACTIVITY key is returned,
 * which will be handled at higher levels by triggering 'JobActivity'
 * autocommands matchin the job name. */
#include "vim.h"

#ifdef FEAT_JOB_CONTROL

#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>

#define POLL_INTERVAL 100 /* Interval used to poll for job activity */
#define KILL_TIMEOUT 25
#define MAX_RUNNING_JOBS 5
#define BUF_SIZE 4096
#define max(x,y) ((x) > (y) ? (x) : (y))

/* Check if the job_id is valid */
#define JOB_CHECK if (job_id <= 0 || job_id > MAX_RUNNING_JOBS || \
	(job = job_table[job_id - 1]) == NULL) \
    return -1

/* Check if the job's process is alive */
#define IS_ALIVE(job) kill(job->pid, 0) != -1

/* Free job in a loop */
#define JOB_FREE \
    close(job->in); \
    close(job->out); \
    close(job->err); \
    for (arg = job->argv; *arg != NULL; arg++) vim_free(*arg); \
    vim_free(job->argv); \
    vim_free(job->name); \
    job_table[job->id - 1] = NULL; \
    vim_free(job);

struct job_T {
    int id, in, out, err, stopped, kill_timeout;
    pid_t pid;
    char_u **argv;
    /* Name is used to match JobActivity autocmds */
    char_u *name;
    /* Fixed-width buffer for stdout/stderr */
    char_u stdout_buf[BUF_SIZE], stderr_buf[BUF_SIZE];
    unsigned int stdout_buf_pos, stderr_buf_pos;
    /* Pending stdin data is stored as a linked list, where each
     * node points the data to be written and tracks its length and
     * position in order to free and advance to the next node correctly. */
    struct in_buf_node_T {
	char_u *data;
	int    len, pos;
	struct in_buf_node_T *next;
    } *stdin_head, *stdin_tail;
} *job_table[MAX_RUNNING_JOBS] = {NULL};


int job_count = 0;
int initialized = FALSE;


/* Poll every running job that has space in its buffers for data. */
    static int
jobs_poll()
{
    int res, mfd, i;
    fd_set rfds, wfds;
    struct timeval tv;
    struct job_T *job;
    struct in_buf_node_T *chunk;
    size_t count, to_write;

    if (!job_count)
	return FALSE;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    mfd = 0;

    /* Iterate through each job, either adding their fds to the appropriate
     * select set or killing stopped jobs */
    for (i = 0; i < MAX_RUNNING_JOBS; i++)
    {
	if ((job = job_table[i]) == NULL) continue;
	if (job->stopped)
	{
	    if (job->kill_timeout-- == KILL_TIMEOUT)
	    {
		/* Vimscript stopped this job, close stdin and
		 * send SIGTERM */ 
		close(job->in);
		kill(job->pid, SIGTERM);
	    }
	    else if (job->kill_timeout == 0)
	    {
		/* We've waited too long, send SIGKILL */
		kill(job->pid, SIGKILL);
	    }
	}
	else
	{
	    /* Only poll stdin if we have something to write */
	    if (job->stdin_head != NULL)
	    {
		FD_SET(job->in, &wfds);
		mfd = max(mfd, job->in);
	    }

	    /* Only poll stdout/stderr if we have room in the buffer */
	    if (job->stdout_buf_pos < BUF_SIZE)
	    {
		FD_SET(job->out, &rfds);
		mfd = max(mfd, job->out);
	    }

	    if (job->stderr_buf_pos < BUF_SIZE)
	    {
		FD_SET(job->err, &rfds);
		mfd = max(mfd, job->err);
	    }
	}
    }

    /* This argument is the maximum fd plus one */
    mfd++;

    /* Check what fds wont block when read/written */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    res = select(mfd, &rfds, &wfds, NULL, &tv);

    if (res <= 0)
    {
	if (res == -1 && errno != EINTR)
	    EMSG(_(e_jobpollerr));
	return FALSE;
    }

    /* Read/write pending data from/to stdfds */
    for (i = 0; i < MAX_RUNNING_JOBS; i++)
    {
	if ((job = job_table[i]) == NULL) continue;

	/* Collect pending stdout/stderr data into the job buffers */
	if (FD_ISSET(job->out, &rfds))
	{
	    count = read(job->out, job->stdout_buf + job->stdout_buf_pos,
		    BUF_SIZE - job->stdout_buf_pos);
	    job->stdout_buf_pos += count;
	}
	if (FD_ISSET(job->err, &rfds))
	{
	    count = read(job->err, job->stderr_buf + job->stderr_buf_pos,
		    BUF_SIZE - job->stderr_buf_pos);
	    job->stderr_buf_pos += count;
	}

	if (FD_ISSET(job->in, &wfds))
	{
	    /* Stdin ready, write as much data as possible */
	    while (job->stdin_head != NULL)
	    {
		chunk = job->stdin_head;
		to_write = chunk->len - chunk->pos;
		count = write(job->in, chunk->data, to_write);

		if (count != to_write)
		{
		    /* Not enough space in the OS buffer, advance position
		     * in the head and break */
		    chunk->pos += count;
		    break;
		}

		/* We have written everything on this chunk, advance
		 * to the next */
		job->stdin_head = chunk->next;
		vim_free(chunk->data);
		vim_free(chunk);
	    }
	}
    }

    return TRUE;
}

/* Set the JOB_ACTIVITY special key into the input buffer */
    static int
job_activity(buf)
    char_u	*buf;
{
    buf[0] = K_SPECIAL;
    buf[1] = KS_EXTRA;
    buf[2] = (int)KE_JOB_ACTIVITY;

    return 3;
}


/* Set the CURSORHOLD special key into the input buffer */
    static int
cursorhold(buf)
    char_u	*buf;
{
    buf[0] = K_SPECIAL;
    buf[1] = KS_EXTRA;
    buf[2] = (int)KE_CURSORHOLD;

    return 3;
}


/* Start a job and return its id. */
    int
job_start(name, argv)
    char_u	*name;
    char_u	**argv;
{
    struct job_T *job = NULL;
    int in[2], out[2], err[2], i;

    if (!initialized)
    {
	signal(SIGCHLD, SIG_IGN);
	initialized = TRUE;
    }

    for (i = 0; i < MAX_RUNNING_JOBS; i++)
	if (job_table[i] == NULL)
	    break;

    if (i == MAX_RUNNING_JOBS)
	/* No more free slots */
	return 0;

    job = (struct job_T *)alloc(sizeof(struct job_T));

    /* Create pipes for the stdio streams */
    pipe(in);
    pipe(out);
    pipe(err);

    if ((job->pid = fork()) == 0)
    {
	/* Child, copy the child parts of the pipes into the appropriate
	 * stdio fds */
	dup2(in[0], 0);
	dup2(out[1], 1);
	dup2(err[1], 2);
    
	/* TODO close all open file descriptors > 2 in a more reliable
	 * way.
	 *
	 * For some options, see:
	 * http://stackoverflow.com/questions/899038/getting-the-highest-allocated-file-descriptor/918469#918469 */
	for (i = 3; i <= 2048; i++)
	    close(i);

	/* Reset ignored signal handlers(does vim ignore other signals?) */
	signal(SIGCHLD, SIG_DFL);

	/* Exec program */
	execvp((const char *)argv[0], (char **)argv);
    }
    else
    {
	/* Close the other sides of the pipes */
	close(in[0]);
	close(out[1]);
	close(err[1]);

	/* Parent, initialize job structure */
	job->id = i + 1;
	job->name = name;
	job->argv = argv;
	job->in = in[1];
	job->out = out[0];
	job->err = err[0];
	job->stdin_head = job->stdin_tail = NULL;
	job->stdout_buf_pos = 0;
	job->stderr_buf_pos = 0;
	job->stopped = FALSE;
	job->kill_timeout = KILL_TIMEOUT;

	/* Insert into the job table */
	job_table[i] = job;
	job_count++;
    }

    /* Return job id */
    return job->id;
}

/* Stop a job */
    int
job_stop(job_id)
    int		job_id;
{
    struct job_T *job;

    JOB_CHECK;
    job->stopped = TRUE;

    return 1;
}

/* Write data to the stdin of a job's process */
    int
job_write(job_id, data, len)
    int		    job_id;
    char_u	    *data;
    unsigned int    len;
{
    struct job_T *job;
    struct in_buf_node_T *chunk;
   
    JOB_CHECK;

    chunk = (struct in_buf_node_T *)alloc(sizeof(struct in_buf_node_T));

    if (chunk == NULL)
	return 0; /* Not enough memory */

    chunk->data = (char_u *)alloc(len);

    if (chunk->data == NULL)
    {
	vim_free(chunk);
	return 0; /* Not enough memory */
    }

    chunk->len = len;
    chunk->pos = 0;
    chunk->next = NULL;
    mch_memmove(chunk->data, data, len);

    if (job->stdin_head == NULL)
	job->stdin_head = job->stdin_tail = chunk;
    else
	job->stdin_tail = job->stdin_tail->next = chunk;

    return 1;
}

/* Cleanup all jobs */
    void
jobs_cleanup()
{
    struct timeval tv;
    int i, kill_now = FALSE;
    struct job_T *job;
    char_u **arg;

    /* Politely ask each job to terminate */
    for (i = 0; i < MAX_RUNNING_JOBS; i++)
	if ((job = job_table[i]) != NULL)
	{
	    close(job->in);
	    kill(job_table[i]->pid, SIGTERM);
	}

    /* Give at most 300 ms for all jobs to exit, then start shooting */
    for (i = 0; i < MAX_RUNNING_JOBS; i++)
    {
	if ((job = job_table[i]) == NULL) continue;
	if (IS_ALIVE(job))
	{
	    if (!kill_now)
	    {
		tv.tv_sec = 0;
		tv.tv_usec = 300000;
		select(0, NULL, NULL, NULL, &tv);
		kill_now = TRUE;
	    }
	    kill(job->pid, SIGKILL);
	}
	JOB_FREE;
    }
}

/*
 * Bridge between vim and the job control module, 'disguised' as a
 * function that returns keys(where one of the special keys is K_JOBACTIVITY
 */
    int
job_activity_poll(buf, maxlen, wtime, tb_change_cnt)
    char_u	*buf;
    int		maxlen;
    long	wtime;
    int		tb_change_cnt;
{
    int		len;
    int		trig_curshold;
    long	ellapsed;

    /* Dont poll for job activity when a timeout is passed */
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
	if (len > 0)
	    return len; /* User-initiated input */

	if (jobs_poll())
	    return job_activity(buf);

	/* We must trigger cursorhold events ourselves. Normally cursorholds
	 * are triggered at a platform-specific lower function when an
	 * infinite timeout is passed, but those won't get the chance
	 * because we never pass infinite timeout in order to poll for
	 * job activity */
	if (trig_curshold)
	{
	    ellapsed += POLL_INTERVAL;
	    if (ellapsed >= p_ut)
		return cursorhold(buf);
	}
    } while (1);
}

/*
 * Invoke the JobActivity autocommand. Called by other layers after we return
 * K_JOBACTIVITY.
 */
    void
job_activity_autocmds()
{
    struct job_T *job;
    char_u **arg;
    list_T *list;
    int i, alive;

    for (i = 0; i < MAX_RUNNING_JOBS; i++)
    {
	if ((job = job_table[i]) == NULL)
	    continue;

	alive = IS_ALIVE(job);
	/* Ignore alive jobs that were stopped or that have no data available
	 * stderr/stdout */
	if (alive && job->stdout_buf_pos == 0 && job->stderr_buf_pos == 0)
	    continue;

	list = list_alloc();
	list_append_number(list, job->id);
	list_append_string(list, job->stdout_buf, job->stdout_buf_pos);
	list_append_string(list, job->stderr_buf, job->stderr_buf_pos);
	job->stdout_buf_pos = job->stderr_buf_pos = 0;
	set_vim_var_list(VV_JOB_DATA, list);
	apply_autocmds(EVENT_JOBACTIVITY, job->name, NULL, TRUE, NULL);

	if (!alive)
	{
	    /* Process exited, free the job memory and remove it from
	     * the table */
	    JOB_FREE;
	    continue;
	}
    }
}
#endif
