#ifndef VM_SPT_H
#define VM_SPT_H

#include <inttypes.h>
#include <hash.h>
#include <string.h>
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"


#define ULIMIT_STACK (1 << 23)

enum page_type
  {
    PAGE_ELF,
    PAGE_SWAP,
    PAGE_MMAP
  };

struct mmap_entry
  {
    struct spt_entry *spte;
    int mapid;
    struct list_elem elem;
  };

struct spt_entry
  {
    enum page_type type;
    void *addr;
    bool pinned;
    bool writeable;
    bool is_present;
    
    struct file *file;
    off_t ofs;
    uint32_t read_bytes;
    uint32_t zero_bytes;

    uint32_t swap_index;
    
    struct hash_elem elem;
  };

extern struct lock filesystem_lock;

void spt_init (struct hash *spt);
void spt_clear (struct hash *spt);
unsigned spte_addr (const struct hash_elem *e, void *aux UNUSED);
bool spte_addr_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);
struct spt_entry * get_spte (const void *);
bool spt_load (struct spt_entry *);
bool spt_stack_growth (void *);
void spt_remove (struct spt_entry *);
bool spt_link_elf (struct file *, off_t, uint8_t *,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable);
bool spt_link_mmap (struct file *, off_t, uint8_t *,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable);

#endif