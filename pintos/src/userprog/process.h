#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

extern struct lock filesys_lock;

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
bool install_page (void *, void *, bool);
void remove_mapid (struct list *, int);

/* File Node */
struct file_node
{
    int file_descriptor;
    struct list_elem elem;
    struct file *file;
};

#endif /* userprog/process.h */
