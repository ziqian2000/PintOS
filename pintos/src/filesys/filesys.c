#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "thread.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_open_root ();
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir = dir_open_root ();
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir = dir_open_root ();
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

/* tz's code begin */

/* Unzip file name slice from *SRCP to SLICE.
   Update **SRCP to the next slice for next call.
   Return 1 if success, 0 if end, -1 if too long.
*/
static int
get_next_slice(char slice[NAME_MAX], const char **srcp)
{
  const char *src = *srcp;
  char *dst = slice;

  /* Continuous slashes equals to one slash (///// = /) */
  while (*src = '/')
    src++;
  if (*src == '\0')
    return 0;

  while (*src != '/' && *src != '\0')
  {
    if (dst < slice + NAME_MAX)
      *dst++ = *src;
    else
      return -1;
    src++;
  }
  *dst = '\0';

  *srcp = src;
  return 1;
}

/* Resolve absolute or relative NAME,
   Store the dir to *DIRP, file_name part to BASE_NAME,
   Return true if success, false if fail.
*/
static bool
name2entry_resolver(const char *name, struct dir **dirp, char base_name[NAME_MAX + 1])
{
  struct inode *inode;
  struct dir *dir;
  int success;
  char slice[NAME_MAX + 1], next_slice[NAME_MAX + 1];
  const char *back_up;

  /* Set initial return value in case of error */
  *dirp = NULL;
  base_name[0] = '\0';

  /* Begin with initial directory */
  if (name[0] == '/' || thread_current()->wd == NULL)
    dir = dir_open_root();
  else
    dir = dir_reopen(thread_current()->wd);
  if (dir == NULL)
  {
    dir_close(dir);
    return false;
  }

  /* Get first name slice */
  back_up = name;
  if (get_next_slice(slice, &back_up) <= 0)
  {
    dir_close(dir);
    return false;
  }

  while ((success = get_next_slice(next_slice, &back_up)) > 0)
  {
    if (!dir_lookup(dir, slice, &inode))
    {
      dir_close(dir);
      return false;
    }

    dir_close(dir);
    dir = dir_open(inode);
    if (dir == NULL)
    {
      dir_close(dir);
      return false;
    }

    strlcpy(slice, next_slice, NAME_MAX + 1);
  }
  if (success < 0)
  {
    dir_close(dir);
    return false;
  }

  *dirp = dir;
  strlcpy (base_name, slice, NAME_MAX + 1);
  return true;
}

/* Resolve relative or absolute NAME to inode
   Return the inode if success, return null pointer if fail
   Close the returned inode is responsible to caller
*/
static struct inode *
name2inode_resolver(const char *name)
{
  if (name[0] == '/' && name[1] == '\0')
  {
    /* root directory */
    return inode_open(ROOT_DIR_SECTOR);
  }
  struct dir *dir;
  char base_name[NAME_MAX + 1];
  if (name2entry_resolver(name, &dir, base_name))
  {
    struct inode *inode;
    dir_lookup(dir, base_name, &inode);
    dir_close(dir);
  }
  return NULL;
}

/* Change current dir to NAME 
   Return true if success, false if fail
*/
bool
filesys_chdir(const char *name)
{
  struct dir *dir = dir_open(name2inode_resolver(name));
  if (dir == NULL)
    return false;
  dir_close(thread_current()->wd);
  thread_current()->wd = dir;
  return true;
}

/* tz's code end */