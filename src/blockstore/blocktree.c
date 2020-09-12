/*
  Routines for transforming flat bundles into their tree representation,
  and storing them into the block store.

*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "sha3.h"

int blocktree_hash_block(unsigned char *hash_out,int hash_len,unsigned char *data,int len)  
{
  sha3_Init256();
  sha3_Update(data,len);
  sha3_Finalize();

  for(int i=0;i<hash_len;i++) hash_out[i]=ctx.s[i>>3][i&7];
  return 0;
  
}
  
