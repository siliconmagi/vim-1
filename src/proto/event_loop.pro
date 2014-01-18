/* event_loop.c */
int ev_next __ARGS((char_u *buf, int maxlen, long wtime, int tb_change_cnt));
void ev_trigger __ARGS((char_u *event, char_u *event_args));
void apply_event_autocmd __ARGS(());
