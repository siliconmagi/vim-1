/* job.c */
int job_start __ARGS((char_u *name, char_u **argv));
int job_stop __ARGS((int job_id));
int job_write __ARGS((int job_id, char_u *data, unsigned int len));
void jobs_cleanup __ARGS((void));
int job_activity_poll __ARGS((char_u *buf, int maxlen, long wtime,
	    int tb_change_cnt));
void job_activity_autocmds __ARGS((void));
