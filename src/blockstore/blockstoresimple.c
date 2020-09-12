/*
  Simple in-memory block store for testing.
  Linear allocation and storage. Really just for testing during early development.
  Doesn't reclaim free space etc.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "blockstore.h"

struct blockstore_simple {
  int blocks_used;
  int blocks_allocated;
  struct block *blocks;
};

void *blockstore_create(unsigned long long initial_size, unsigned long long maximum_size,char *device)
{
  struct blockstore_simple *bs=calloc(sizeof(struct blockstore_simple),1);

  if (!bs) return NULL;

  bs->blocks=calloc(sizeof(struct block),initial_size/sizeof(struct block));

  if (!bs->blocks) return NULL;
  bs->blocks_allocated=initial_size/sizeof(struct block);
  bs->blocks_used=0;
		    
  return (void *)bs;
}

int blockstore_retrieve(void *bs_in,unsigned char *hash,int hash_len,unsigned char *block, int* block_len)
{
  struct blockstore_simple *bs=bs_in;
  if (!bs) return -1;

  for(int i=0;bs->blocks_used;i++) {
    if (hash_len==bs->blocks[i].hash_len)
      if (!memcmp(hash,bs->blocks[i].hash,hash_len)) {
	// Found
	block=bs->blocks[i].data;
	*block_len=bs->blocks[i].data_len;
	return 0;
      }
  }

  // Not present
  return -1;
}


int blockstore_store(void *bs_in,unsigned char *hash,int hash_len,unsigned char *block, int block_len)
{
  struct blockstore_simple *bs=bs_in;
  if (!bs) return -1;

  for(int i=0;i<bs->blocks_used;i++) {
    if (hash_len==bs->blocks[i].hash_len)
      if (!memcmp(hash,bs->blocks[i].hash,hash_len))
	// Already stored
	return 0;    
  }

  // Append if space permits
  if (bs->blocks_used<bs->blocks_allocated) {
    memcpy(bs->blocks[bs->blocks_used].data,block,block_len);
    memcpy(bs->blocks[bs->blocks_used].hash,hash,hash_len);
    bs->blocks[bs->blocks_used].hash_len=hash_len;
    bs->blocks[bs->blocks_used].data_len=block_len;
    bs->blocks_used++;
    return 0;
  }

  // No space
  return -1;
  
}
