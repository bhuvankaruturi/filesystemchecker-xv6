#include "sys/mman.h"
#include "sys/stat.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "fcntl.h"
#include "string.h"

#define stat xv6_stat  // avoid clash with host struct stat
#define dirent xv6_dirent  // avoid clash with host struct stat
#include "include/types.h"
#include "include/fs.h"
#include "include/stat.h"
#undef stat
#undef dirent

char bitarr[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
// begin address of the filesystem image in memory
char* addr;
// macro to check if the bit at blocknum is set
#define CHECKBIT(bitmap, blocknum) ((*(bitmap + blocknum/8)) & (bitarr[blocknum%8]))

// set the bit pointed by blocknum
void setbit (char* bitmap, uint blocknum) {
  bitmap = (bitmap + blocknum/8);
  *bitmap = (*bitmap) | (bitarr[blocknum%8]);
}

// unset the bit pointed by blocknum
void unsetbit (char* bitmap, uint blocknum) {
  bitmap = (bitmap + blocknum/8);
  *bitmap = (*bitmap) & (~bitarr[blocknum%8]);
}

// print the error message to standard error and exit the program
void handleerror(char* errormsg) {
  fprintf(stderr, errormsg);
  exit(1);
}

// check the directory for inconsistencies
void checkdirectory(struct dinode* dip, uint ino,  char* inodebitmap, char* inodebitmap_cp, short* refcounts) {
  int n = dip[ino].size/sizeof(struct xv6_dirent);
  // entries per block
  int epb = BSIZE/sizeof(struct xv6_dirent);
  struct xv6_dirent* de;
  int i;
  // foundself is the '.' entry
  int foundself = 0;
  // foundpar is the '..' entry
  int foundpar = 0;
  int counter = 0;
  // indirect block address
  uint* inaddr = (uint *)(addr + (dip[ino].addrs[NDIRECT] * BSIZE));
  while (counter < NDIRECT + NINDIRECT) {
   if (counter < NDIRECT)
     // entries in blocks pointed by direct addresses
     de = (struct xv6_dirent*)(addr + dip[ino].addrs[counter] * BSIZE);
   else {
     // entries in blocks pointed by indirect addresses
     de = (struct xv6_dirent*)(addr + (*inaddr * BSIZE));
     inaddr++;
   }
   int limit = epb;
   if (n < epb) limit = n;
    // loop over all the directory entries in the current block
    for (i = 0; i < limit; i++, de++) {
      if (de->inum != 0) {
        if (!CHECKBIT(inodebitmap, de->inum))
          handleerror("ERROR: inode referred to in directory but marked free\n");
        else {
          if (strcmp(de->name, ".") == 0) {
	          foundself = 1;
	          if (de->inum != ino) 
	            handleerror("ERROR: directory not properly formatted\n");
	        }  
          else if (strcmp(de->name, "..")==0) {
	          if (ino == ROOTINO) {
	            if (de->inum != ino) 
	               handleerror("ERROR: root directory does not exist\n");
	          }
	        foundpar = 1;
	        }
          else if (dip[de->inum].type==T_DIR) {
	          if (CHECKBIT(inodebitmap_cp, de->inum))
              unsetbit(inodebitmap_cp, de->inum);
            else
              handleerror("ERROR: directory appears more than once in file system\n");
          }
          else if (dip[de->inum].type == T_FILE && strcmp(de->name, "") != 0)
	          refcounts[de->inum]++;
          } 
          unsetbit(inodebitmap_cp, de->inum);
      }
    }
    if (n <= epb) break;
    counter++;
    n -= epb;
  }
  if (!foundself || !foundpar)
    handleerror("ERROR: directory not properly formatted\n");
}

int main(int argc, char* argv[]) {
  if (argc < 2) 
    handleerror("Usage: fcheck <file_system_image>\n");
  
  int fd;
  int i;
  struct stat st;
  char* bitmap;
  char* inodebitmap;
  short* refcounts;
  
  fd = open(argv[1], O_RDONLY);
  if (fd == -1) 
    handleerror("Could not open the given filesystem image\n");
  
  if (fstat(fd, &st) == -1) 
    handleerror("Could not retrieve the file system image size\n");
  
  addr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  
  if (addr == MAP_FAILED) 
    handleerror("mmap failed\n");
  
  // read in the super block of the filesystem
  struct superblock* sb;
  sb = (struct superblock*) (addr + (1 * BSIZE));
  
  int bitblocks = sb->size/(BSIZE * 8) + 1;
  int metablocks = sb->ninodes/IPB + 3 + bitblocks;
  //int datablocks = sb->size - metablocks;
  struct dinode* dip = (struct dinode*) (addr + IBLOCK((uint)0)*BSIZE);
  
  // bitmap for all the blocks in the image
  bitmap = (char *)(addr + (IBLOCK((uint)0) * BSIZE) + (sb->ninodes/IPB + 1) * BSIZE);
  char* bitmap_cp;
  bitmap_cp = (char *) malloc(bitblocks * BSIZE);
  memcpy(bitmap_cp, bitmap, bitblocks * BSIZE);
  
  // inode bitmap and refcounts
  inodebitmap = (char *) malloc ((sb->ninodes/8) + 1);
  refcounts = (short *) malloc (sb->ninodes * sizeof(short));
  // build the inode bitmap
  for (i = 0; i < sb->ninodes; i++) {
    refcounts[i] = 0;
    if (dip[i].type == 0) unsetbit(inodebitmap, i);
    else setbit(inodebitmap, i);
  }
  char* inodebitmap_cp = (char *) malloc ((sb->ninodes/8)+1);
  memcpy(inodebitmap_cp, inodebitmap, (sb->ninodes/8)+1);
  
  // loop over all the inodes in the filesystem and check for inconsistencies
  for (i = 0; i < sb->ninodes; i++) {
    // check if the inode is of valid type
    if (dip[i].type < 0 || dip[i].type > 3) 
      handleerror("ERROR: bad inode\n");
    // inode is valid - check its contents
    else {
      int j;
      // DIRECT blocks
      for (j = 0; j < NDIRECT+1; j++) {
        if (dip[i].addrs[j] != 0) {
	        if (dip[i].addrs[j] < metablocks || dip[i].addrs[j] >= sb->size) {
	          if (j == NDIRECT) 
	            handleerror("ERROR: bad indirect address in inode\n");
	          handleerror("ERROR: bad direct address in inode\n");
	        } 
	        // if the block nums are valid, check if the bitmap is set correctly
	        else {
	          if (!CHECKBIT(bitmap, dip[i].addrs[j]))
	            handleerror("ERROR: address used by inode but marked free in bitmap\n"); 
	          else {
	            if (CHECKBIT(bitmap_cp, dip[i].addrs[j]))
	              unsetbit(bitmap_cp, dip[i].addrs[j]); 
	            else
		            handleerror("ERROR: direct address used more than once\n");
	          }
	        }    
	      }
      }
      // INDIRECT blocks
      if (dip[i].addrs[NDIRECT] != 0) {
	      int k;
        uint* inaddr = (uint *)(addr + (dip[i].addrs[NDIRECT] * BSIZE));
        for (k = 0; k < NINDIRECT; k++, inaddr++) {
          if (*inaddr != 0) {
          if (*inaddr < metablocks || *inaddr > sb->size)
            handleerror("ERROR: bad indirect address in inode\n");
          // if the block nums are valid, check if the bitmap is set correctly
          else {
            if (!CHECKBIT(bitmap, *inaddr))
              handleerror("ERROR: address used by inode but marked free in bitmap\n");
            else {
              if (CHECKBIT(bitmap_cp, *inaddr))
                unsetbit(bitmap_cp, *inaddr);
              else
                handleerror("ERROR: indirect address used more than once\n");
            }
          }
	      }
      }
    }
    // if inode num is ROOTINO, check whether it is a directory
    if (i == ROOTINO) {
      if (dip[i].type != T_DIR) 
        handleerror("ERROR: root directory does not exist\n");
    }
    // check for consistency in the contents of directories
    if (dip[i].type == T_DIR)
      checkdirectory(dip,(uint)i,  inodebitmap, inodebitmap_cp, refcounts);
    }
  }
  // check the bitmap for inconsistencies
  for (i = metablocks; i < sb->size; i++) {
    if (CHECKBIT(bitmap_cp, i)) 
      handleerror("ERROR: bitmap marks block in use but it is not in use\n");
  }
  // check the inode bitmap for inconsistencies
  for (i = ROOTINO; i < sb->ninodes; i++) {
    if (CHECKBIT(inodebitmap, i)) {
      if (CHECKBIT(inodebitmap_cp, i)) 
        handleerror("ERROR: inode marked use but not found in a directory\n");
    }
    // check the reference count of  each file is correct
    if (dip[i].type == T_FILE) {
      if (dip[i].nlink != refcounts[i])
        handleerror("ERROR: bad reference count for file\n");
    }
  }
  return 0;
}

