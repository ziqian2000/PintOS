#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "devices/input.h"
#include "pagedir.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/exception.h"

#include "vm/frame.h"
#include "vm/swap.h"
#include "vm/page.h"


static void syscall_handler (struct intr_frame *);

struct lock filesys_lock;

void sys_halt (void);
void sys_exit (int);
tid_t sys_exec (const char *);
int sys_wait (tid_t);
bool sys_create(const char*, unsigned);
bool sys_remove(const char*);
int sys_open(const char*);
int sys_filesize(int);
void sys_seek(int, unsigned);
unsigned sys_tell(int);
void sys_close(int);
static int sys_read(int, void *, unsigned);
int sys_write(int, const void*, unsigned);
void sys_exception_exit(void);
static int sys_mmap(int fd, void *addr);  
static void sys_munmap(int map);


static struct file_node* find_file_node(struct thread *t, int file_descriptor);
static void check_user(const uint8_t *uaddr);
static int get_user_bytes(void *src, void *dst, size_t size);
static int get_user (const uint8_t *uaddr);
static void get_syscall_arg(struct intr_frame *, uint32_t *, int);


static struct spt_entry *
check_and_pin_addr (const void *addr, void *esp)
{
  struct spt_entry *spte = get_spte (addr);
  if (spte != NULL)
  {
    spt_load (spte);
  }
  else
  {
    if (is_stack_growth (addr, esp))
    {
      if (!spt_stack_growth (addr)) 
        sys_exit (-1);
    } 
    else 
    {
      sys_exit (-1);
    }
  }
  return spte;
}

static void
check_and_pin_buffer(const void *uaddr, unsigned int len, void *esp, bool write)
{
  for (const void *addr = uaddr; addr < uaddr + len; ++addr)
  {
    if (!is_valid_user_addr (addr)) sys_exit (-1);
    struct spt_entry *spte = check_and_pin_addr (addr, esp);
    if (spte != NULL && write && !spte->writeable) sys_exit (-1);
  }
}

static void 
unpin_addr (void *addr)
{
  struct spt_entry *spte = get_spte (addr);
  if (spte != NULL) 
    spte->pinned = false;
}

static void
unpin_buffer (void *uaddr, unsigned int len)
{
  for (void *addr = uaddr; addr < uaddr + len; ++addr)
    unpin_addr (addr);
}



void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  if(!is_user_vaddr(f->esp))
  {
    sys_exception_exit();
  }


  uint32_t call_number = *((uint32_t *) f->esp);


  switch (call_number)
  {
  case SYS_HALT: // 0
    {
      sys_halt();
      NOT_REACHED();
      break;
      
    }
  case SYS_EXIT: // 1
    {
      int exitcode;
      get_user_bytes(f->esp + 4, &exitcode, sizeof(exitcode));

      sys_exit(exitcode);
      NOT_REACHED();


      break;
      
    }

  case SYS_EXEC: // 2
    {
      void* cmdline;
      get_user_bytes(f->esp + 4, &cmdline, sizeof(cmdline));

      int return_code = sys_exec((const char*) cmdline);
      f->eax = (uint32_t) return_code;

      break;
    }

  case SYS_WAIT: // 3
    {
      tid_t tid;
      get_user_bytes(f->esp + 4, &tid, sizeof(tid_t));

      int ret = sys_wait(tid);
      f->eax = (uint32_t) ret;


      break;
    }

  case SYS_CREATE: // 4
    {
      const char* filename;
      unsigned initial_size;
      bool return_code;

      get_user_bytes(f->esp + 4, &filename, sizeof(filename));
      get_user_bytes(f->esp + 8, &initial_size, sizeof(initial_size));

      return_code = sys_create(filename, initial_size);
      f->eax = return_code;

      break;
    }

  case SYS_REMOVE: // 5
    {
      const char* filename;
      bool return_code;

      get_user_bytes(f->esp + 4, &filename, sizeof(filename));

      return_code = sys_remove(filename);
      f->eax = return_code;


      break;
    }

  case SYS_OPEN: // 6
    {
      const char* filename;
      int return_code;

      get_user_bytes(f->esp + 4, &filename, sizeof(filename));

      return_code = sys_open(filename);
      f->eax = return_code;


      break;
    }

  case SYS_FILESIZE: // 7
    {
      int fd, return_code;
      get_user_bytes(f->esp + 4, &fd, sizeof(fd));

      return_code = sys_filesize(fd);
      f->eax = return_code;


      break;
    }

  case SYS_READ: // 8
    {

      int fd, return_code;
      void *buffer;
      unsigned size;

      get_user_bytes(f->esp + 4, &fd, sizeof(fd));
      get_user_bytes(f->esp + 8, &buffer, sizeof(buffer));
      get_user_bytes(f->esp + 12, &size, sizeof(size));

      check_and_pin_buffer (buffer, size, f->esp, true);
      return_code = sys_read(fd, buffer, size);
      f->eax = return_code;
      unpin_buffer (buffer, size);
      break;
    }

  case SYS_WRITE: // 9
    {
      int fd, return_code;
      const void *buffer;
      unsigned size;

      get_user_bytes(f->esp + 4, &fd, sizeof(fd));
      get_user_bytes(f->esp + 8, &buffer, sizeof(buffer));
      get_user_bytes(f->esp + 12, &size, sizeof(size));

      return_code = sys_write(fd, buffer, size);
      f->eax = (uint32_t) return_code;


      break;
    }

  case SYS_SEEK: // 10
    {
      int fd;
      unsigned position;

      get_user_bytes(f->esp + 4, &fd, sizeof(fd));
      get_user_bytes(f->esp + 8, &position, sizeof(position));

      sys_seek(fd, position);
      break;
    }

  case SYS_TELL: // 11
    {
      int fd;
      unsigned return_code;

      get_user_bytes(f->esp + 4, &fd, sizeof(fd));

      return_code = sys_tell(fd);
      f->eax = (uint32_t) return_code;
      break;
    }

  case SYS_CLOSE: // 12
    {
      int fd;
      get_user_bytes(f->esp + 4, &fd, sizeof(fd));

      sys_close(fd);
      break;
    }
  case SYS_MMAP:
    {
      uint32_t syscall_args[4];
      get_syscall_arg(f, syscall_args, 2);
      f->eax = sys_mmap(syscall_args[0], (void *)syscall_args[1]);
      break;
    }
  case SYS_MUNMAP:
    {
      uint32_t syscall_args[4];
      get_syscall_arg(f, syscall_args, 1);
      sys_munmap(syscall_args[0]);
      break;
    }
    
  default:
    {
      NOT_REACHED();
      // sys_exception_exit();
    }

  }
}

/**************************************** Syscall ****************************************/

int
sys_wait(tid_t tid)
{
  if(tid == -1) return -1;
  else return process_wait(tid);
}

tid_t
sys_exec(const char *cmd_line)
{
  check_user((const uint8_t*) cmd_line);

  //lock_acquire(&filesys_lock);
  tid_t tid = process_execute(cmd_line);
  struct thread *t = get_thread_by_tid(tid);
  ASSERT(t != NULL);

  sema_down(&t->exec_done_sema1);

  t->parent = thread_current();
  tid_t ret = t->tid;

  ASSERT(t->exec_done_sema2.value == 0);
  sema_up(&t->exec_done_sema2);

  //lock_release(&filesys_lock);
  return ret;
}

void
sys_halt(void)
{
  shutdown_power_off();
}

int
sys_open(const char *file_name)
{
  check_user((const uint8_t*) file_name);
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
sys_write(int fd, const void *buffer, unsigned size)
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
  if(fn)
    file_seek(fn->file, position);
  lock_release(&filesys_lock);
}

void 
sys_exit(int status)
{
  struct thread *cur = thread_current();
  cur->ret_val = status;
  thread_exit();
}

/* Exit due to invalid memory access. 
   FILESYS_LOCK will be released if held. */
void 
sys_exception_exit()
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
      if(f->file_descriptor == file_descriptor)
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
  {
    sys_exception_exit();
  }
}

/* Reads consecutive SIZE bytes of user memory from SRC and writes into DST. 
   Returns the number of read bytes. 
   If it fails, the current process will be terminated with return value -1. */
static int
get_user_bytes(void *src, void *dst, size_t size)
{
  int value;
  for(size_t i = 0; i < size; i++)
  {
    value = get_user(src + i);
    if(value == -1)
    {
      sys_exception_exit();
    }
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
 


//VM
static struct file_node *
get_fdstruct(int fd)
{
  struct list_elem *e;

  for (e = list_begin(&thread_current()->file_nodes);
       e != list_end(&thread_current()->file_nodes);
       e = list_next(e))
  {
    struct file_node *f = list_entry(e, struct file_node, elem);
    if (f->file_descriptor == fd)
      return f;
  }
  return NULL;
}

static int
sys_mmap(int fd, void *addr)
{
  struct file_node *fd_s = get_fdstruct(fd);
  int r = file_length (fd_s->file);
  if (!fd_s || !is_valid_user_addr (addr) || ((uint32_t) addr % PGSIZE) != 0 || r == 0)
  {
    return -1;
  }
  
  struct file *file = file_reopen (fd_s->file);
  off_t ofs = 0;
  uint32_t read_bytes = r;
  uint32_t zero_bytes = 0;
  while (read_bytes > 0)
  {
    uint32_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    uint32_t page_zero_bytes = PGSIZE - page_read_bytes;
    
    if (!spt_link_mmap (file, ofs, addr, page_read_bytes, page_zero_bytes, true)) return -1;
    
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    ofs += page_read_bytes;
    addr += PGSIZE;
  }

  return thread_current ()->mapid++;
}

static void
sys_munmap(int map)
{
  remove_mapid (&thread_current ()->mmap_list, map);
}

static void
get_syscall_arg(struct intr_frame *f, uint32_t *buffer, int argc)
{

  uint32_t *ptr;
  for (ptr = (uint32_t *)f->esp + 1; argc > 0; ++buffer, --argc, ++ptr)
  {
    check_and_pin_addr (ptr, (void *)sizeof(uint32_t));
    *buffer = *ptr;
  }
}

static int
sys_read(int fd, void *buffer, unsigned size)
{ 
  lock_acquire(&filesys_lock);
  if (fd == 0) return 0;
  else if (fd == 1)
  {
    lock_release(&filesys_lock);
    sys_exception_exit();
    return -1;
  }
  else
  {
    struct file_node *fd_s = get_fdstruct(fd);
    if (!fd_s)
    {
      lock_release(&filesys_lock);
      return -1;
    }
    int r = file_read(fd_s->file, buffer, size);
    lock_release(&filesys_lock);
    return r;
  }
}

