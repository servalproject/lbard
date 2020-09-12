/*
  Routines for transforming flat bundles into their tree representation,
  and storing them into the block store.

*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "blockstore.h"

// Allow a block tree to have more than one block store.
// Blocks will be preferentially stored into the first store,
// and the others will be used as a heirarchy of storage, with
// blocks ejected from one to make space moved to the next one down.
// It a blockstore is not marked writeable, then it will be used only
// to answer queries, but not to store anything.
// This can be used to allow a blocktree to read from a cache of blocks
// received from other peers, without being obligated to store them
// indefinitely, or have to worry about garbage collection in the cache.
#define BT_MAX_BLOCKSTORES 16

struct blocktree {
  // The top node of the tree
  unsigned char top_hash[BS_MAX_HASH_SIZE];
  int top_hash_len;
  
  // Block stores that hold the blocks
  void *blockstores[BT_MAX_BLOCKSTORES];
  int blockstore_writeable[BT_MAX_BLOCKSTORES];
  int blockstore_count;
};

#include "sha3.h"

int blocktree_hash_block(unsigned char *salt,int salt_len,
			 unsigned char *hash_out,int hash_len,unsigned char *data,int len)  
{
  sha3_Init256();
  sha3_Update(salt,salt_len);
  sha3_Update(data,len);
  sha3_Finalize();

  for(int i=0;i<hash_len;i++) hash_out[i]=ctx.s[i>>3][i&7];
  return 0;
  
}

void *blocktree_open(void *blockstore,unsigned char *hash,int hash_len)
{
  struct blocktree *bt=calloc(sizeof(struct blocktree),1);
  bt->blockstore_count=1;
  bt->blockstore_writeable[0]=1;
  bt->blockstores[0]=blockstore;
  memcpy(bt->top_hash,hash,hash_len);
  bt->top_hash_len=hash_len;
  return bt;
}
