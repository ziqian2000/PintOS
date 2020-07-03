#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
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

typedef int pid_t;

static void syscall_handler(struct intr_frame *);


static int get_syscall_type(struct intr_frame *);
static void get_syscall_arg(struct intr_frame *, uint32_t *, int);

struct lock filesys_lock;

static bool sys_create(const char *file, unsigned initial_size);
static bool sys_remove(const char *file);
static int sys_open(const char *file);
static void sys_close(int fd);
void sys_exit(int status);
static int sys_write(int fd, const void *buffer, unsigned size);
static int sys_read(int fd, void *buffer, unsigned size);
static int sys_filesize(int fd);
static pid_t sys_exec(const char *cmd_line);
static int sys_wait(pid_t pid);
static void sys_seek(int fd, unsigned position);
static unsigned sys_tell(int fd);
static void sys_halt(void);
static int sys_mmap(int fd, void *addr);
static void sys_munmap(int map);
void sys_exception_exit(void);

static void check_user(const uint8_t *uaddr);
static int get_user (const uint8_t *uaddr);



static struct spt_entry *
check_and_pin_addr (void *addr, void *esp)
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
check_and_pin_buffer(void *uaddr, unsigned int len, void *esp, bool write)
{
  for (const void *addr = uaddr; addr < uaddr + len; ++addr)
  {
    if (!is_valid_user_addr (addr)) sys_exit (-1);
    struct spt_entry *spte = check_and_pin_addr (addr, esp);
    if (spte != NULL && write && !spte->writeable) sys_exit (-1);
  }
}

static void
check_and_pin_string (const void *str, void *esp)
{
  check_and_pin_addr (str, esp);
  while (*(char *)str != 0)
  {
    str = (char *)str + 1;
    check_and_pin_addr (str, esp);
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

static void 
unpin_string (void *str)
{
  unpin_addr (str);
  while (*(char *)str != 0)
  {
    str = (char *)str + 1;
    unpin_addr (str);
  }
}

static void  
valid_uaddr (const void * uaddr, unsigned int len)
{
  for (const void * addr = uaddr; addr < uaddr + len ; ++addr)
    if ((!addr) ||
       !(is_user_vaddr (addr)) ||
       get_spte (addr) == NULL)
       {
        sys_exit(-1);
        return;
       }
}

void syscall_init(void)
{
  lock_init(&filesys_lock);
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler(struct intr_frame *f UNUSED)
{
  uint32_t syscall_args[4];

  int type = get_syscall_type(f);
  check_and_pin_addr((const void *)f->esp, f->esp);
  switch (type)
  {
  case SYS_CREATE:
    get_syscall_arg(f, syscall_args, 2);
    check_and_pin_string ((const void *)syscall_args[0], f->esp);
    f->eax = sys_create((char *)syscall_args[0], syscall_args[1]);
    ASSERT (is_valid_user_addr (syscall_args[0]));
    unpin_string ((void *)syscall_args[0]);
    break;
  case SYS_REMOVE:
    get_syscall_arg(f, syscall_args, 1);
    check_and_pin_string ((const void *)syscall_args[0], f->esp);
    f->eax = sys_remove((char *)syscall_args[0]);
    break;
  case SYS_OPEN:
    get_syscall_arg(f, syscall_args, 1);
    check_and_pin_string ((const void *)syscall_args[0], f->esp);
    f->eax = sys_open((char *)syscall_args[0]);
    ASSERT (is_valid_user_addr (syscall_args[0]));
    unpin_string ((void *)syscall_args[0]);
    break;
  case SYS_CLOSE:
    get_syscall_arg(f, syscall_args, 1);
    sys_close(syscall_args[0]);
    break;
  case SYS_EXIT:
    get_syscall_arg(f, syscall_args, 1);
    sys_exit(syscall_args[0]);
    break;
  case SYS_WRITE:
    get_syscall_arg(f, syscall_args, 3);
    check_and_pin_buffer ((void *)syscall_args[1], syscall_args[2], f->esp, false);
    f->eax = sys_write(syscall_args[0], (void *)syscall_args[1], syscall_args[2]);
    unpin_buffer ((void *)syscall_args[1], syscall_args[2]);
    break;
  case SYS_READ:
    get_syscall_arg(f, syscall_args, 3);
    check_and_pin_buffer ((void *)syscall_args[1], syscall_args[2], f->esp, true);
    f->eax = sys_read(syscall_args[0], (void *)syscall_args[1], syscall_args[2]);
    unpin_buffer ((void *)syscall_args[1], syscall_args[2]);
    break;
  case SYS_FILESIZE:
    get_syscall_arg(f, syscall_args, 1);
    f->eax = sys_filesize(syscall_args[0]);
    break;
  case SYS_EXEC:
    get_syscall_arg(f, syscall_args, 1);
    check_and_pin_string ((const void *)syscall_args[0], f->esp);
    f->eax = sys_exec((char *)syscall_args[0]);
    ASSERT (is_valid_user_addr (syscall_args[0]));
    unpin_string ((void *)syscall_args[0]);
    break;
  case SYS_WAIT:
    get_syscall_arg(f, syscall_args, 1);
    f->eax = sys_wait(syscall_args[0]);
    break;
  case SYS_SEEK:
    get_syscall_arg(f, syscall_args, 2);
    sys_seek(syscall_args[0], syscall_args[1]);
    break;
  case SYS_TELL:
    get_syscall_arg(f, syscall_args, 1);
    f->eax = sys_tell(syscall_args[0]);
    break;
  case SYS_MMAP:
    get_syscall_arg(f, syscall_args, 2);
    f->eax = sys_mmap(syscall_args[0], (void *)syscall_args[1]);
    break;
  case SYS_MUNMAP:
    get_syscall_arg(f, syscall_args, 1);
    sys_munmap(syscall_args[0]);
    break;
  case SYS_HALT:
    sys_halt();
    break;
  }
  unpin_addr (f->esp);
}

/* Get the type of system call */
static int
get_syscall_type(struct intr_frame *f)
{
  valid_uaddr(f->esp, sizeof(uint32_t));
  return *((uint32_t *)f->esp);
}

/* Get arguments which have been pushed into stack in lib/user/syscall.c */
static void
get_syscall_arg(struct intr_frame *f, uint32_t *buffer, int argc)
{

  uint32_t *ptr;
  for (ptr = (uint32_t *)f->esp + 1; argc > 0; ++buffer, --argc, ++ptr)
  {
    check_and_pin_addr (ptr, sizeof(uint32_t));
    *buffer = *ptr;
  }
}

static void
check_string(const char *str)
{
  const char *ptr;
  for (ptr = str; valid_uaddr(ptr, 1), *ptr; ++ptr)
    ;
  valid_uaddr(ptr, 1);
}

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

/* Implements of syscalls */
static bool
sys_create(const char *file, unsigned initial_size)
{

  /* Check the string */
  check_string(file);

  /* Call filesys */
  lock_acquire(&filesys_lock);
  bool r = filesys_create(file, initial_size);
  lock_release(&filesys_lock);
  return r;
}

static bool
sys_remove(const char *file)
{

  check_string(file);

  lock_acquire(&filesys_lock);
  bool r = filesys_remove(file);
  lock_release(&filesys_lock);
  return r;
}

static int
sys_open(const char *file)
{
  check_string(file);
  lock_acquire(&filesys_lock);
  struct file *r = filesys_open(file);
  if (!r)
  {
    lock_release(&filesys_lock);
    return -1;
  }
  struct file_node *fd_s = malloc(sizeof(struct file_node));

  fd_s->file_descriptor = ++thread_current()->max_fd;
  fd_s->file = r;
  //strlcpy(fd_s->name, file, strlen(file));
  list_push_back(&thread_current()->file_nodes, &fd_s->elem);

  lock_release(&filesys_lock);
  return fd_s->file_descriptor;
}

static void
sys_close(int fd)
{
  if (fd < 2)
    sys_exit(-1);
  struct file_node *fd_s = get_fdstruct(fd);
  if (!fd_s)
    sys_exit(-1);

  lock_acquire(&filesys_lock);

  file_close(fd_s->file);
  list_remove(&fd_s->elem);
  free(fd_s);

  lock_release(&filesys_lock);
}

void sys_exit(int status)
{

  while (!list_empty(&thread_current()->file_nodes))
  {
    struct list_elem *e = list_pop_front(&thread_current()->file_nodes);
    struct file_node *fd_s = list_entry(e, struct file_node, elem);
    file_close(fd_s->file);
    free(fd_s);
  }
  thread_current()->ret_val = status;

  thread_exit();

}

static int
sys_write(int fd, const void *buffer, unsigned size)
{
  lock_acquire(&filesys_lock);
  if (fd == 0)
  {
    lock_release(&filesys_lock);
    sys_exit(-1);
    return -1;
  }
  else if (fd == 1)
  {
    putbuf((const char *)buffer, size);
    lock_release(&filesys_lock);
    return size;
  }
  else
  {    
    struct file_node *fd_s = get_fdstruct(fd);
    if (!fd_s)
    {
      lock_release(&filesys_lock);
      return -1;
    }
    int r = file_write(fd_s->file, buffer, size);
    lock_release(&filesys_lock);
    return r;
  }
}

static int
sys_read(int fd, void *buffer, unsigned size)
{ 
  lock_acquire(&filesys_lock);
  if (fd == 0)
  {

    return 0;
  }
  else if (fd == 1)
  {
    lock_release(&filesys_lock);
    sys_exit(-1);
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

static int
sys_filesize(int fd)
{
  struct file_node *fd_s = get_fdstruct(fd);
  if (!fd_s)
    return -1;
  lock_acquire(&filesys_lock);
  int r = file_length(fd_s->file);
  lock_release(&filesys_lock);
  return r;
}


static void 
check_user(const uint8_t *uaddr)
{
  if(get_user(uaddr) == -1)
  {
    sys_exception_exit();
  }
}


static int
get_user (const uint8_t *uaddr)
{
  if(!is_user_vaddr(uaddr)) return -1;

  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

static pid_t
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

static int
sys_wait(pid_t pid)
{
  if(pid == -1) return -1;
  return process_wait(pid);
}

static void
sys_seek(int fd, unsigned position)
{
  struct file_node *fd_s = get_fdstruct(fd);
  if (!fd_s)
    return;
  lock_acquire(&filesys_lock);
  file_seek(fd_s->file, position);
  lock_release(&filesys_lock);
}

static unsigned
sys_tell(int fd)
{
  struct file_node *fd_s = get_fdstruct(fd);
  if (!fd_s)
    return -1;
  lock_acquire(&filesys_lock);
  unsigned r = file_tell(fd_s->file);
  lock_release(&filesys_lock);
  return r;
}

static void
sys_halt(void)
{
  shutdown_power_off();
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

void 
sys_exception_exit()
{
  if(lock_held_by_current_thread(&filesys_lock))
    lock_release(&filesys_lock);
  sys_exit(-1);
}

//VM
/*
static int
sys_mmap(int fd, void *addr)
{
  lock_acquire(&filesys_lock);
  struct file_node *fn = find_file_node(thread_current(), fd);
  int r = file_length(fn->file);
  lock_release(&filesys_lock);

  if (!fd || !is_valid_user_addr (addr) || ((uint32_t) addr % PGSIZE) != 0 || r == 0)
  {
    lock_release(&filesys_lock);
    return -1;
  }
  
  struct file *file = file_reopen (fn->file);
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
*/