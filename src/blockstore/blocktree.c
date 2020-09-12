/*
  Routines for transforming flat bundles into their tree representation,
  and storing them into the block store.

*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "blockstore.h"
#include "blocktree.h"

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

  // The salt that is used in this block tree for hashing
  unsigned char salt[BT_MAX_SALT_LEN];
  int salt_len;
  
  // Block stores that hold the blocks
  void *blockstores[BT_MAX_BLOCKSTORES];
  int blockstore_writeable[BT_MAX_BLOCKSTORES];
  int blockstore_count;
};

#include "sha3.h"

int blocktree_unpack_node(unsigned char *block,int len,struct blocktree_node_unpacked *n)
{
  /*
    The packed format of the a node is:

    1 byte - low nybl = pointer count, high nybl = leaf count
    Then pointers follow.
    Then leaves follow.
   */

  if (len<1) {
    n->pointer_count=0;
    n->leaf_count=0;
    return 0;
  }

  // Get counts of fields
  n->pointer_count=block[0]&0xf;
  n->leaf_count=block[0]>>4;

  // Make sure the block doesn't have invalidly large entry counts
  if (n->pointer_count>8) return BTERR_TREE_CORRUPT;
  if (n->leaf_count>2) return BTERR_TREE_CORRUPT;
  
  int p=0;
  int l=0;
  
  int ofs=1;

  while(ofs<len) {
    if (p<n->pointer_count) {
      // Extract pointer to sub-child
      n->pointers[l].child_bits=block[ofs++];
      memcpy(n->pointers[p].child_hash,&block[ofs],BS_HASH_SIZE); ofs+=BS_HASH_SIZE;
      p++;
    } else if (l) {
      // Extract bundle leaf node
      memcpy(n->leaves[p].bid,&block[ofs],32); ofs+=32;
      n->leaves[p].version=0;
      for(int i=0;i<8;i++) {
	n->leaves[p].version<<=8;
	n->leaves[p].version|=block[ofs++];	
      }
      memcpy(n->leaves[p].manifest_hash,&block[ofs],BS_HASH_SIZE); ofs+=BS_HASH_SIZE;
      memcpy(n->leaves[p].payload_hash,&block[ofs],BS_HASH_SIZE); ofs+=BS_HASH_SIZE;      
      l++;
    } else break;
  }
  if (ofs!=len) {
    fprintf(stderr,"ERROR: Corrupt block.\n");
    return BTERR_TREE_CORRUPT;
  }

  return BTOK_SUCCESS;
}

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

void *blocktree_open(void *blockstore,
		     unsigned char *salt, int salt_len,
		     unsigned char *hash,int hash_len)
{
  // Sanity check parameters
  if (salt_len<0||salt_len>BT_MAX_SALT_LEN) return NULL;
  if (hash_len<1||hash_len>BS_MAX_HASH_SIZE) return NULL;
  
  struct blocktree *bt=calloc(sizeof(struct blocktree),1);
  bt->blockstore_count=1;
  bt->blockstore_writeable[0]=1;
  bt->blockstores[0]=blockstore;
  memcpy(bt->top_hash,hash,hash_len);
  bt->top_hash_len=hash_len;
  memcpy(bt->salt,salt,salt_len);
  bt->salt_len=salt_len;
  return bt;
}

// Used for returning results in blocktree_find_bundle_record()
struct blocktree_node res;

struct blocktree_node *blocktree_find_bundle_record(void *blocktree,unsigned char *bid)
{
  /*
    Look up a node in the block tree.

    Each node potentially contains a mix of pointers to child-nodes and/or 
    leaf-node entries.  (struct blocktree_child_pointer and struct blocktree_bundle_leaf,
    respectively.

    The first byte of the block indicates how many of each, using a single byte that 
    contains the number of each in each nybl.

    In order to optimise transfers, the blocks have a canonical representation, where
    pointers come before leaf nodes, and entries are sorted by the BID space that the
    correspond to.

    If a block has become too full to fit everything, then the leaf node with lowest
    BID is ejected into a child node. This is repeated until such time as the block is
    no longer over full.
  */

  unsigned char i;
  struct blocktree *bt=blocktree;
  
  if (!bt) return NULL;

  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
  
  // Start at the top of the tree
  int bit_offset=0;

  // Start with top node
  int node_hash_len=bt->top_hash_len;
  unsigned char node_hash[BS_MAX_HASH_SIZE];
  memcpy(node_hash,bt->top_hash,bt->top_hash_len);

  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
  
  // Limit search to maximum depth implied by by 256-bit BIDs
  while(bit_offset<=256) {

    fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
    
    unsigned char found_node=0;
    for(i=0;i<bt->blockstore_count;i++) {
      if (!blockstore_retrieve(bt->blockstores[i],node_hash,node_hash_len,res.block,&res.block_len)) {
	found_node=1;
	break;
      }
    }
    if (!found_node) {
      // We are missing part of the tree required to get to the node.
      res.status=BTERR_INTERMEDIATE_MISSING;
      res.depth=bit_offset/3;
      memcpy(res.node,node_hash,node_hash_len);
      return &res;
    }

    // Ok, we have the node, so now take a look in it
    if (blocktree_unpack_node(res.block,res.block_len,&res.unpacked)) {
      // Found it, but can't unpack the node
      res.status=BTERR_TREE_CORRUPT;
      res.depth=bit_offset/3;
      memcpy(res.node,node_hash,node_hash_len);
      return &res;
    }

    // Ok, we have the node.
    for(i=0;i<res.unpacked.leaf_count;i++) {
      if (!memcmp(res.unpacked.leaves[i].bid,bid,32)) {
	// Found it!
	res.status=BTOK_FOUND;
	res.leaf_num=i;
	res.depth=bit_offset/3;
	memcpy(res.node,node_hash,node_hash_len);
	return &res;
      }
    }
    // Nope, its not in a leaf node in this block.
    // Find the correct pointer, and follow that, or return
    // BTERR_NOT_IN_TREE if it is definitely not there.
    unsigned char next_bits=0;
    for(i=0;i<3;i++) {
      next_bits=next_bits<<1;
      if (bid[(bit_offset+i)>>3]&(0x80>>(i&7))) next_bits|=1;
    }

    for(i=0;i<res.unpacked.pointer_count;i++) {
      if (res.unpacked.pointers[i].child_bits==next_bits) {
	// Found pointer
	memcpy(node_hash,res.unpacked.pointers[i].child_hash,BS_HASH_SIZE);
	bit_offset+=3;
	break;
      }
    }
    if (i==res.unpacked.pointer_count) {
      res.status=BTERR_NOT_IN_TREE;
      res.depth=bit_offset/3;
      return &res;
    }
    
  }

  // Searched to base of tree, and still can't find it, so it isn't there.
  res.status=BTERR_NOT_IN_TREE;
  res.depth=bit_offset/3;
  return &res;  
}

void blocktree_hash_data(void *blocktree,unsigned char *hash_out,unsigned char *data,int len)
{
  struct blocktree *bt=blocktree;
  blocktree_hash_block(bt->salt,bt->salt_len,hash_out,BS_HASH_SIZE,data,len);
}

int blocktree_insert_bundle(void *blocktree,
			    char *bid_hex,char *version_asc,
			    unsigned char *manifest_encoded,int manifest_encoded_len,
			    unsigned char *body,int body_len)
{
  struct blocktree_node *n;
  unsigned char i;
  char hex[3];

  struct blocktree_bundle_leaf leaf;
  
  // Parse hex BID to binary
  for(i=0;i<32;i++) {
    hex[0]=bid_hex[i*2+0];
    hex[1]=bid_hex[i*2+1];
    hex[2]=0;
    leaf.bid[i]=atoi(hex);
  }
  leaf.version=strtoll(version_asc,NULL,10);
  blocktree_hash_data(blocktree,leaf.manifest_hash,manifest_encoded,manifest_encoded_len);
  blocktree_hash_data(blocktree,leaf.payload_hash,body,body_len);
  
  // Now find out if it is in the tree
  n=blocktree_find_bundle_record(blocktree,leaf.bid);
  printf("n->status=%d @ depth %d\n",n->status,n->depth);

  switch(n->status) {
  case BTERR_NOT_IN_TREE:
    // Its not in the tree, so we can just add it into this node,
    // possibly causing this node to split.
    blocktree_node_insert_leaf(blocktree,n,&leaf);
    blocktree_node_pack(blocktree,n);
    blocktree_node_write(blocktree,n);
    break;
  case BTOK_FOUND:
    // Check the version of the stored one. If the same or older, then
    // we can just return with nothing to do.
    // If its older, then we need to delete it and update the tree back up
    // to the root.
    // XXX This might also result in the node unsplitting.
    if (leaf.version>n->unpacked.leaves[n->leaf_num].version) {
      // Update the leaf entry and write back
      // (No need to worry about node splitting, since we are only replacing
      // content.)
      memcpy(n->unpacked.leaves[n->leaf_num].manifest_hash,leaf.manifest_hash,BS_HASH_SIZE);
      memcpy(n->unpacked.leaves[n->leaf_num].payload_hash,leaf.payload_hash,BS_HASH_SIZE);
      n->unpacked.leaves[n->leaf_num].version = leaf.version;
      blocktree_node_pack(blocktree,n);
      blocktree_node_write(blocktree,n);
    } else {
      // We are trying to insert an older version, so nothing to do. We keep
      // the newer version.
    }
    break;
  case BTERR_TREE_CORRUPT: 
  case BTERR_INTERMEDIATE_MISSING:
  default:
      return n->status;      
  }
  
  // Completed successfully
  return BTOK_SUCCESS;
}
