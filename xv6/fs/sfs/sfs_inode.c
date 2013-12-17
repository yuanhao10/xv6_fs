#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "buf.h"
#include "sfs.h"
#include "sfs_inode.h"
#include "inode.h"
#include "vfs.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
static void sfs_itrunc(struct inode*);
static const struct inode_ops sfs_node_driops;
static const struct inode_ops sfs_node_fileops;

/*
 * sfs_get_ops - return function addr of fs_node_dirops/sfs_node_fileops
 */
static const struct inode_ops *
sfs_get_ops(uint type) {
    switch (type) {
    case SFS_TYPE_DIR:
        return &sfs_node_dirops;
    case SFS_TYPE_FILE:
        return &sfs_node_fileops;
    }
    panic("invalid file type %d.\n", type);
}

// Read the super block.
void
readsb(int dev, struct sfs_super *sb)
{
  struct buf *bp;
  
  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;
  
  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks. 

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;
  struct sfs_super sb;

  bp = 0;
  readsb(dev, &sb);
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb.ninodes));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  struct sfs_super sb;
  int bi, m;

  readsb(dev, &sb);
  bp = bread(dev, BBLOCK(b, sb.ninodes));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk immediately after
// the superblock. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->flags.
//
// An inode and its in-memory represtative go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, iput() frees if
//   the link count has fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() to find or
//   create a cache entry and increment its ref, iput()
//   to decrement ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when the I_VALID bit
//   is set in ip->flags. ilock() reads the inode from
//   the disk and sets I_VALID, while iput() clears
//   I_VALID if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode. The I_BUSY flag indicates
//   that the inode is locked. ilock() sets I_BUSY,
//   while iunlock clears it.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.

struct {
  struct spinlock lock;
  struct sfs_inode inode[NINODE];
} icache;

void
sfs_iinit(void)
{
  initlock(&icache.lock, "icache");
}

static struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate a new inode with the given type on device dev.
// A free inode has a type of zero.
struct inode*
sfs_ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct sfs_dinode *dip;
  struct sfs_super sb;
  struct inode *node;

  readsb(dev, &sb);

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      node = sfs_iget(dev, inum);
      vop_init(node, sfs_get_ops(type));
      return 
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
void
sfs_iupdate(struct inode *ip)
{
  struct sfs_inode *sin = vop_info(ip, sfs_inode);
  struct buf *bp;
  struct sfs_dinode *dip;

  bp = bread(sin->dev, IBLOCK(sin->inum));
  dip = (struct sfs_dinode*)bp->data + sin->inum%IPB;
  dip->type = sin->type;
  dip->major = sin->major;
  dip->minor = sin->minor;
  dip->nlink = sin->nlink;
  dip->size = sin->size;
  memmove(dip->addrs, sin->addrs, sizeof(sin->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
sfs_iget(uint dev, uint inum)
{
  struct inode *ip, *empty;
  struct sfs_inode *sip = vop_info(ip, sfs_inode);
  struct sfs_inode *sempty = vop_info(empty, sfs_inode);

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(sip = &icache.inode[0]; sip < &icache.inode[NINODE]; sip++){
    if(sip->ref > 0 && sip->dev == dev && sip->inum == inum){
      sip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && sip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  sip->dev = dev;
  sip->inum = inum;
  sip->ref = 1;
  sip->flags = 0;
  vop_init(ip, sfs_get_ops());
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
sfs_idup(struct inode *ip)
{
  struct sfs_inode *sip = vop_info(ip, sfs_inode);
  acquire(&icache.lock);
  sip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
sfs_ilock(struct inode *ip)
{
  struct sfs_inode *sin = vop_info(ip, sfs_inode);
  struct buf *bp;
  struct sfs_dinode *dip;

  if(sin == 0 || sin->ref < 1)
    panic("ilock");

  acquire(&icache.lock);
  while(sin->flags & I_BUSY)
    sleep(sin, &icache.lock);
  sin->flags |= I_BUSY;
  release(&icache.lock);

  if(!(sin->flags & I_VALID)){
    bp = bread(sin->dev, IBLOCK(sin->inum));
    dip = (struct dinode*)bp->data + sin->inum%IPB;
    sin->type = dip->type;
    sin->major = dip->major;
    sin->minor = dip->minor;
    sin->nlink = dip->nlink;
    sin->size = dip->size;
    memmove(sin->addrs, dip->addrs, sizeof(sin->addrs));
    brelse(bp);
    sin->flags |= I_VALID;
    if(sin->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
sfs_iunlock(struct inode *ip)
{
  struct sfs_inode *sin = vop_info(ip, sfs_inode);
  if(sin == 0 || !(sin->flags & I_BUSY) || sin->ref < 1)
    panic("iunlock");

  acquire(&icache.lock);
  sin->flags &= ~I_BUSY;
  wakeup(sin);
  release(&icache.lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
void
sfs_iput(struct inode *ip)
{
  struct sfs_inode *sin = vop_info(node, sfs_inode);
  acquire(&icache.lock);
  if(sin->ref == 1 && (sin->flags & I_VALID) && sin->nlink == 0){
    // inode has no links: truncate and free inode.
    if(sin->flags & I_BUSY)
      panic("iput busy");
    sin->flags |= I_BUSY;
    release(&icache.lock);
    sfs_itrunc(ip);
    sin->type = 0;
    sfs_iupdate(ip);
    acquire(&icache.lock);
    sin->flags = 0;
    wakeup(sin);
  }
  sin->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
sfs_iunlockput(struct inode *ip)
{
  iunlock(ip);
  sfs_iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are 
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct sfs_inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void
sfs_itrunc(struct inode *ip)
{
  struct sfs_inode *sin = vop_info(ip, sfs_inode);
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(sin->addrs[i]){
      bfree(sin->dev, sin->addrs[i]);
      sin->addrs[i] = 0;
    }
  }
  
  if(sin->addrs[NDIRECT]){
    bp = bread(sin->dev, sin->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(sin->dev, a[j]);
    }
    brelse(bp);
    bfree(sin->dev, sin->addrs[NDIRECT]);
    sin->addrs[NDIRECT] = 0;
  }

  sin->size = 0;
  sfs_iupdate(ip);
}

// sfs_openfile - open file (no use)
static int
sfs_openfile(struct inode *node, uint open_flags) {
    return 0;
}

// sfs_close - close (no use)
static int
sfs_close(struct inode *node) {
    return 0;
}

// Copy stat information from inode.
void
sfs_stati(struct inode *ip, struct stat *st)
{
  struct sfs_inode *sin = vop_info(node, sfs_inode); 
  st->dev = sin->dev;
  st->ino = sin->inum;
  st->type = sin->type;
  st->nlink = sin->nlink;
  st->size = sin->size;
}

//PAGEBREAK!
// Read data from inode.
int
sfs_readi(struct inode *ip, char *dst, uint off, uint n)
{
  struct sfs_inode *sin = vop_info(node, sfs_inode);
  uint tot, m;
  struct buf *bp;

  if(sin->type == T_DEV){
    if(sin->major < 0 || sin->major >= NDEV || !devsw[sin->major].read)
      return -1;
    return devsw[sin->major].read(sin, dst, n);
  }

  if(off > sin->size || off + n < off)
    return -1;
  if(off + n > sin->size)
    n = sin->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(sin->dev, bmap(sin, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
int
sfs_writei(struct inode *ip, char *src, uint off, uint n)
{
  struct sfs_inode *sin = vop_info(node, sfs_inode);
  uint tot, m;
  struct buf *bp;

  if(sin->type == T_DEV){
    if(sin->major < 0 || sin->major >= NDEV || !devsw[sin->major].write)
      return -1;
    return devsw[sin->major].write(sin, src, n);
  }

  if(off > sin->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(sin->dev, bmap(sin, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(bp->data + off%BSIZE, src, m);
    log_write(bp);
    brelse(bp);
  }

  if(n > 0 && off > sin->size){
    sin->size = off;
    sfs_iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
sfs_dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(sfs_readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return sfs_iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
sfs_dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    sfs_iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(sfs_readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(sfs_writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");
  
  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;
  struct sfs_inode *sinp = vop_info(ip, sfs_inode);
  struct sfs_inode *sinnext = vop_info(next, sfs_inode);

  if(*path == '/')
    ip = sfs_iget(ROOTDEV, ROOTINO);
  else
    ip = sfs_idup(proc->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(sinp->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    sfs_iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
sfs_namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
sfs_nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

// The sfs specific DIR operations correspond to the abstract operations on a inode.
static const struct inode_ops sfs_node_dirops = {
    .vop_magic                      = VOP_MAGIC,
    .vop_close                      = sfs_close,
    .vop_fstat                      = sfs_stati,
    .vop_fsync                      = sfs_iupdate,
    .vop_ref_dec                    = sfs_iput,
    .vop_trunc                      = sfs_itrunc,
    .vop_namei                      = sfs_namei,
    .vop_nameiparent                = sfs_nameiparent,
    .vop_dirlink                    = sfs_dirlink,
    .vop_dirlookup                  = sfs_dirlookup,
}; 
// The sfs specific FILE operations correspond to the abstract operations on a inode.
static const struct inode_ops sfs_node_fileops = {
    .vop_magic                      = VOP_MAGIC,
    .vop_open                       = sfs_openfile,
    .vop_close                      = sfs_close,
    .vop_read                       = sfs_readi,
    .vop_write                      = sfs_writei,
    .vop_fstat                      = sfs_stati,
    .vop_fsync                      = sfs_iupdate
    .vop_ref_dec                    = sfs_iput,
    .vop_trunc                      = sfs_itrunc,
};