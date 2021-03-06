#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

//  #define USERPROG // stupid VSCode
//  #define VM
//  #define FILESYS

#include <debug.h>
#include <list.h>
#include <hash.h>
#include <stdint.h>
#include "fixed_point.h"
#include "synch.h"

/* Guided by https://stackoverflow.com/questions/52472084/pintos-userprog-all-tests-fail-is-kernel-vaddr */
// bool threading_started;

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread
  {
   /* Owned by thread.c. */
   tid_t tid;                          /* Thread identifier. */
   enum thread_status status;          /* Thread state. */
   char name[16];                      /* Name (for debugging purposes). */
   uint8_t *stack;                     /* Saved stack pointer. */
   int priority;                       /* Priority. */
   struct list_elem allelem;           /* List element for all threads list. */

   /* Shared between thread.c and synch.c. */
   struct list_elem elem;              /* List element. */

   int64_t remaining_sleeping_ticks;   /* The number of remaining ticks before awakened. */
   int base_priority;                  /* Base priority. */
   struct list locks_holding;          /* Holded locks. */
   struct lock *locks_acquiring;       /* Lock being acquired and waited for. */

   int nice;                           /* Niceness. */
   fixed_t recent_CPU;                 /* An estimate of the CPU time the thread has used recently. */


// vvvvvvvvvv USERPROG vvvvvvvvvv

   /* Owned by userprog/process.c. */
   uint32_t *pagedir;                  /* Page directory. */
   struct hash spt;

   int ret_val;                        /* Return value of process. */
   int max_fd;                         /* Maximum value of file descriptor. */
   bool ret_val_saved;                 /* Whether the return value has been saved. */
   bool dont_print_exit_msg;           /* Don't print exit msg because "The message is optional when a process fails to load." */
   struct list file_nodes;             /* List of file nodes. */
   struct list child_ret_data;         /* List of return value of children. */
   struct thread *parent;              /* Parent process. NULL if it's an orphan. */
   struct semaphore exec_done_sema1;   /* Used to inform parent of execution result. Means that its parent is waiting for it. */
   struct semaphore exec_done_sema2;   /* Used to inform parent of execution result. Means that it is waiting for its parent. */
   struct semaphore exit_sema;         /* Used to inform parent of its exit. */
   struct file* file_self;              /* Executable of itself. */

   /* mmap use */
    int mapid;
    struct list mmap_list;

// ^^^^^^^^^^ USERPROG ^^^^^^^^^^

   /* Owned by thread.c. */
   unsigned magic;                     /* Detects stack overflow. */
  };

struct return_data
{
   tid_t tid;                           /* That thread's tid. */
   int ret_val;                         /* Return value. */
   struct list_elem elem;               /* List element. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void sleeping_thread_check(struct thread *, void *);

bool thread_cmp_by_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);

void thread_adjust_for_priority(struct thread *);
void thread_update_priority(struct thread *);
void thread_donate_priority(struct thread *);

void thread_mlfqs_update_load_avg_and_recent_cpu(void);
void thread_mlfqs_update_priority(struct thread *);
void thread_mlfqs_increase_recent_CPU(void);

struct thread* get_thread_by_tid(tid_t);
int get_child_ret_val_by_tid(struct thread *, tid_t);

void thread_preempt(struct thread *);

void make_children_orphans(struct thread*);

#endif /* threads/thread.h */
