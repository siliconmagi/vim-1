/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

typedef enum { UserInput, DeferredCall } MessageType;

typedef struct message_T
{ 
  struct message_T * next;
  MessageType type;
  void *data;
} message_T;

typedef struct input_data_T
{
    int character;
    int mod_mask;
    int mouse_row;
    int mouse_col;
} input_data_T;

void input_notify();
void input_acquire();
void input_release();
void queue_ensure();
void queue_push(MessageType, void *);
message_T * queue_shift();

#endif
