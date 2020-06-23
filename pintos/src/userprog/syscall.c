#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "filesys/filesys.h"

static void syscall_handler (struct intr_frame *);

struct lock filesys_lock;



void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  if(!is_user_vaddr(f->esp))
    sys_exception_exit(-1);

  uint32_t call_number = *((uint32_t *) f->esp);
  switch (call_number)
  {
    // todo : more cases here
  case SYS_HALT:
    {
      sys_halt();
      break;
    }
  default:
    {
      sys_exception_exit(-1);
    }

  }
}

/**************************************** Syscall ****************************************/

void
sys_halt(void)
{
  shutdown_power_off();
}

int
sys_open(const char *file_name)
{
  check_user(file_name);
  struct file_node *fn = (struct file_node*) malloc (sizeof(struct file_node));
  struct thread *current_thread = thread_current();
  int ret_val;

  lock_acquire(&filesys_lock);

  fn->file = filesys_open(file_name);
  if(fn->file == NULL)
  {
    ret_val = -1;
    free(fn);
  }
  else
  {
    ret_val = fn->file_descriptor = ++current_thread->max_fd;
    list_push_back(&current_thread->file_nodes, &fn->elem);
  }

  lock_release(&filesys_lock);
  return ret_val;
}

void 
sys_close(int fd)
{
  lock_acquire(&filesys_lock);
  struct file_node *fn = find_file_node(thread_current(), fd);
  if(fn)
  {
    file_close(fn->file);
    list_remove(&fn->elem);
    free(fn);
  }
  lock_release(&filesys_lock);
}

int
sys_read(int fd, void *buffer, unsigned size)
{
  /* Verify the memory [buffer, buffer + size). */
  check_user((uint8_t *) buffer);
  check_user((uint8_t *) buffer + size - 1);
  int ret_val;

  lock_acquire(&filesys_lock);

  if(fd == 0) // read from keyboard
  {
    for(int i = 0; i < size; i++)
    {
      if(!put_user(buffer + i, input_getc()))
        exception_exit();
    }
    ret_val = size;
  }
  else // read from file
  {
    struct file_node *fn = find_file_node(thread_current(), fd);
    if(fn)
      ret_val = file_read(fn->file, buffer, size);
    else
      ret_val = -1;
    
  }

  lock_release(&filesys_lock);
  return ret_val;
}

int
sys_write(int fd, void *buffer, unsigned size)
{
  /* Verify the memory [buffer, buffer + size). */
  check_user((uint8_t *) buffer);
  check_user((uint8_t *) buffer + size - 1);
  int ret_val;

  lock_acquire(&filesys_lock);

  if(fd == 1) // write to console
  {
    putbuf(buffer, size);
    ret_val = size;
  }
  else // write to file
  {
    struct file_node *fn = find_file_node(thread_current(), fd);
    if(fn)
      ret_val = file_write(fn->file, buffer, size);
    else
      ret_val = -1;
    
  }

  lock_release(&filesys_lock);
  return ret_val;
}

int
sys_filesize(int fd)
{
  lock_acquire(&filesys_lock);
  struct file_node *fn = find_file_node(thread_current(), fd);

  if(!fd)
  {
    lock_release(&filesys_lock);
    return -1;
  }

  int ret_val = file_length(fn->file);
  lock_release(&filesys_lock);
  return ret_val;
}

bool
sys_create(const char *file_name, unsigned size)
{
  bool ret_val;
  check_user((uint8_t *)file_name);
  lock_acquire(&filesys_lock);
  ret_val = filesys_create(file_name, size);
  lock_release(&filesys_lock);
  return ret_val;
}

bool
sys_remove(const char *file_name)
{
  bool ret_val;
  check_user((uint8_t *)file_name);
  lock_acquire(&filesys_lock);
  ret_val = filesys_remove(file_name);
  lock_release(&filesys_lock);
  return ret_val;
}

unsigned 
sys_tell(int fd)
{
  lock_acquire(&filesys_lock);
  struct file_node *fn = find_file_node(thread_current(), fd);
  unsigned ret_val;
  if(fn)
    ret_val = file_tell(fn->file);
  else
    ret_val = -1;
  lock_release(&filesys_lock);
  return ret_val;
}

void
sys_seek(int fd, unsigned position)
{
  lock_acquire(&filesys_lock);
  struct file_node *fn = find_file_node(thread_current(), fd);
  unsigned ret_val;
  if(fn)
    file_seek(fn->file, position);
  lock_release(&filesys_lock);
}

void 
sys_exit(int status)
{
  printf("%s: exit(%d)\n", thread_current()->name, status);
  // todo
}

/* Exit due to invalid memory access. 
   FILESYS_LOCK will be released if held. */
static void 
exception_exit()
{
  if(lock_held_by_current_thread(&filesys_lock))
    lock_release(&filesys_lock);
  sys_exit(-1);
}

/**************************************** Utility Method ****************************************/

/* Finds the file node with file_descriptor FILE_DESCRIPTOR held by thread T.
   Returns NULL if not found. */
static struct file_node*
find_file_node(struct thread *t, int file_descriptor)
{
  ASSERT(t != NULL);

  struct list_elem *e;
  if(!list_empty(&t->file_nodes))
  {
    for(e = list_begin(&t->file_nodes); e != list_end(&t->file_nodes); e = list_next(e))
    {
      struct file_node *f = list_entry(e, struct file_node, elem);
      if(f->file_descriptor = file_descriptor)
        return f;
    }
  }
  return NULL;
}

/* Check if UADDR is valid. 
   If not, just exit. */
static void 
check_user(const uint8_t *uaddr)
{
  if(get_user(uaddr) == -1)
    exception_exit();
}

/* Reads consecutive SIZE bytes of user memory from SRC and writes into DST. 
   Returns the number of read bytes. 
   If it fails, the current process will be terminated with return value -1. */
static void
get_user_bytes(void *src, void *dst, size_t size)
{
  int value;
  for(size_t i = 0; i < size; i++)
  {
    value = get_user(src + i);
    if(value == -1) exception_exit();
    *(char *)(dst + i) = value & 0xff;
  }
  return (int)size;
}

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user (const uint8_t *uaddr)
{
  if(!is_user_vaddr(uaddr)) return -1;

  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}
 
/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  if(!is_user_vaddr(udst)) return -1;

  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}