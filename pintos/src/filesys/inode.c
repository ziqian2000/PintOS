#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"

#ifdef FS

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_SECTOR_MAXN 123
#define SECTOR_MAXN 125
#define POINTER_MAXN ((off_t) (BLOCK_SECTOR_SIZE / sizeof (block_sector_t)))

#define INODE_BYTE_MAXN ((DIRECT_SECTOR_MAXN                \
                     + POINTER_MAXN                    \
                     + POINTER_MAXN * POINTER_MAXN) \
                    * BLOCK_SECTOR_SIZE)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t sectors[SECTOR_MAXN]; 
    enum inode_type type;
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

static inline size_t 
get_hierarchy(block_sector_t i)
{
  return (i >= DIRECT_SECTOR_MAXN) + (i >= DIRECT_SECTOR_MAXN + 1);
}

/* In-memory inode. */
struct inode 
  {
    struct lock lock;
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */

    struct lock deny_write_lock;
    struct condition no_write;
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    int write_cnt;
  };


/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct lock open_inodes_lock;


/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}

/* Initializes an inode of TYPE of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns the inode created
  */
struct inode *
inode_create (block_sector_t sector, enum inode_type type)
{
  struct inode_disk *disk_inode = NULL;
  struct inode *inode;

  struct cache_entry *entry = cache_lock(sector, EXCLUSIVE);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = cache_setzero(entry);
  disk_inode->type = type;
  disk_inode->length = 0;
  disk_inode->magic = INODE_MAGIC;
  cache_dirty(entry);
  cache_unlock(entry);

  inode = inode_open(sector);
  if(inode == NULL) free_map_release(sector);

  return inode;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  lock_acquire(&open_inodes_lock);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode->open_cnt++;

          lock_release(&open_inodes_lock);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    {
      lock_release(&open_inodes_lock);
      return NULL;
    }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  lock_init(&inode->lock);
  lock_init(&inode->deny_write_lock);
  cond_init(&inode->no_write);

  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

  lock_release(&open_inodes_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
  {
    lock_acquire(&open_inodes_lock);
    inode->open_cnt++;
    lock_release(&open_inodes_lock);
  }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

enum inode_type type_of_inode(const struct inode *inode)
{
  struct cache_entry *entry = cache_lock(inode->sector, NON_EXCLUSIVE);
  struct inode_disk *disk_inode = cache_read(entry);
  enum inode_type type = disk_inode->type;
  cache_unlock(entry);
  return type;
}


/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */

void inode_erase_recursive(block_sector_t sector, int h)
{
  if(h > 0)
  {
    struct cache_entry *entry = cache_lock(sector, EXCLUSIVE); 
    block_sector_t *ptrs = cache_read(entry);
    for(int i=0;i< POINTER_MAXN; i++)
      if(ptrs[i])
        inode_erase_recursive(sector, h - 1);
    
    cache_unlock(entry);
  }

  cache_free(sector);
  free_map_release(sector);
}

void inode_erase(struct inode *inode)
{
  struct cache_entry *entry = cache_lock(inode->sector, EXCLUSIVE);
  struct inode_disk *disk_inode = cache_read(entry);

  for(int i = 0; i < SECTOR_MAXN; i++)
    if(disk_inode->sectors[i])
      inode_erase_recursive(disk_inode->sectors[i], get_hierarchy(i));

  cache_unlock(entry);
  inode_erase_recursive(inode->sector, 0); // erase this
}

void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */

  lock_acquire(&open_inodes_lock);
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
      lock_release(&open_inodes_lock);

      /* Deallocate blocks if removed. */
      if (inode->removed) 
          inode_erase(inode);
      free (inode); 
    }
  else lock_release(&open_inodes_lock);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}


/*
  The sector_id might point to an indirect sector, 
  return the hierarchy 
  offsets in the sectors from each hierarchy is stored in offset[]
*/
static int 
resolve_offset(off_t sector_id, size_t offset[])
{
  if(sector_id < DIRECT_SECTOR_MAXN) // direct sector
  {
    offset[0] = sector_id;
    return 1;  
  }

  sector_id -= DIRECT_SECTOR_MAXN;

  /*hierarchy = 1*/
  if(sector_id < POINTER_MAXN)
  {
    offset[0] = DIRECT_SECTOR_MAXN + sector_id/POINTER_MAXN;
    offset[1] = sector_id % POINTER_MAXN;
    return 2;
  }

  sector_id -= POINTER_MAXN;

  if(sector_id < POINTER_MAXN * POINTER_MAXN)
  {
    offset[0] = DIRECT_SECTOR_MAXN + 1 + sector_id/(POINTER_MAXN * POINTER_MAXN);
    offset[1] = sector_id / POINTER_MAXN;
    offset[2] = sector_id % POINTER_MAXN;
    return 3;
  }
  NOT_REACHED();
}


/*
  fetch the data block in the inode with offset
  return teh cache entry where that data lies
  return NULL if fails
*/
static bool 
get_data_block(struct inode *inode, off_t offset, bool allocate, struct cache_entry **entry)
{

  size_t offsets[3];
  size_t hierarchy = resolve_offset(offset / BLOCK_SECTOR_SIZE, offsets);
  size_t h = 0;

  size_t cnt = 0;

  block_sector_t current_sector = inode->sector;
  while(true)
  {
    struct cache_entry *current_entry = cache_lock(current_sector, NON_EXCLUSIVE);

    uint32_t *current_data = cache_read(current_entry);

    if(current_data[offsets[h]])    // already allocated
      {
        current_sector = current_data[offsets[h]];

        if(h == hierarchy - 1)  // arrive at the target block
        {  
          /*  read-ahead optimization
          if(offsets[h] + 1 < DIRECT_SECTOR_MAXN)
          {
            uint32_t next_sector = current_data[offsets[h] + 1];
            if(next_sector && next_sector < block_size(fs_device))
              cache_readahead(next_sector);
          }
          */
          
          cache_unlock(current_entry);

          *entry = cache_lock(current_sector, NON_EXCLUSIVE);
          return true;
        }
        cache_unlock(current_entry);
        h++;
        continue;
      }
    
    cache_unlock(current_entry);
    
    if(!allocate)
    {
      *entry = NULL;
      return true;
    }

    // try to allocate a new block
    current_entry = cache_lock(current_sector, EXCLUSIVE);
    current_data = cache_read(current_entry);

    if(current_data[offsets[h]])  //someone else already allocated this block, try again
    {
        cache_unlock(current_entry);
        continue; 
    }
    // allocate a new block
    if(!free_map_allocate(&current_data[offsets[h]]))   // fail
    {
      cache_unlock(current_entry);
      *entry = NULL;
      return false;
    }
    

    cache_dirty(current_entry);

    struct cache_entry *new_entry = cache_lock(current_data[offsets[h]], EXCLUSIVE);
    cache_setzero(new_entry);

    cache_unlock(current_entry);

    if(h == hierarchy - 1)  // reach direct sector
    {
      *entry = new_entry;
      return true;
    }
    // go down to next hierarchy  
    cache_unlock(new_entry);
  }
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      struct cache_entry *entry;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0 || !get_data_block(inode, offset, false, &entry))    // ALLOCATE = FALSE: no extention will be done
        break;
      
      if(entry == NULL)   // set to zero if read beyond EOF
        memset(buffer + bytes_read, 0, chunk_size);
      else 
      {
        const uint8_t *data = cache_read(entry);
        memcpy(buffer + bytes_read, data + sector_ofs, chunk_size);
        cache_unlock(entry);
      }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

static void update_inode_length(struct inode *inode, off_t length)
{
  if(length > inode_length(inode))
  {
    struct cache_entry *entry = cache_lock(inode->sector, EXCLUSIVE);
    struct inode_disk *disk_inode = cache_read(entry);
    if(length > disk_inode->length)
    {
      disk_inode->length = length;
      cache_dirty(entry);
    }
    cache_unlock(entry);
  }
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  // deny-write check
  lock_acquire(&inode->deny_write_lock);
  if (inode->deny_write_cnt)
  {
    lock_release(&inode->deny_write_lock);
    return 0;
  }

  inode->write_cnt++;
  lock_release(&inode->deny_write_lock);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      struct cache_entry *entry;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = INODE_BYTE_MAXN - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      
      if (chunk_size <= 0 || !get_data_block(inode, offset, true, &entry)){  // ALLOCATE = TRUE: allocate new block if write beyond EOF
        break;
      }
      uint8_t *data = cache_read(entry);
      memcpy(data + sector_ofs, buffer + bytes_written, chunk_size);
      cache_dirty(entry);
      cache_unlock(entry);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  update_inode_length(inode, offset);

  lock_acquire(&inode->deny_write_lock);
  if(--inode->write_cnt == 0)
    cond_signal(&inode->no_write, &inode->deny_write_lock);
  lock_release(&inode->deny_write_lock);
  
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire(&inode->deny_write_lock);
  while(inode->write_cnt > 0)
    cond_wait(&inode->no_write, &inode->deny_write_lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release(&inode->deny_write_lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  lock_acquire(&inode->deny_write_lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release(&inode->deny_write_lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct cache_entry *entry = cache_lock(inode->sector, NON_EXCLUSIVE);
  struct inode_disk *disk_inode = cache_read(entry);

  off_t len = disk_inode->length;
  cache_unlock(entry);
  return len;
}

/* Locks INODE. */
void
inode_lock (struct inode *inode) 
{
  lock_acquire(&inode->lock);
}

/* Releases INODE's lock. */
void
inode_unlock (struct inode *inode) 
{
  lock_release(&inode->lock);
}

#else

#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[125];               /* Not used. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      if (free_map_allocate (sectors, &disk_inode->start)) 
        {
          block_write (fs_device, sector, disk_inode);
          if (sectors > 0) 
            {
              static char zeros[BLOCK_SECTOR_SIZE];
              size_t i;
              
              for (i = 0; i < sectors; i++) 
                block_write (fs_device, disk_inode->start + i, zeros);
            }
          success = true; 
        } 
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          free_map_release (inode->data.start,
                            bytes_to_sectors (inode->data.length)); 
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}


#endif