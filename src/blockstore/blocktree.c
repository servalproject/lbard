/*
  Routines for transforming flat bundles into their tree representation,
  and storing them into the block store.

*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sync.h"
#include "lbard.h"
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

int compare_node_child_pointer(const void *a,const void *b)
{
  const struct blocktree_child_pointer *aa=a;
  const struct blocktree_child_pointer *bb=b;

  if (aa->child_bits<bb->child_bits) return -1;
  if (aa->child_bits>bb->child_bits) return 1;
  return 0;  
}

int compare_bundle_leaf(const void *a,const void *b)
{
  const struct blocktree_bundle_leaf *aa=a;
  const struct blocktree_bundle_leaf *bb=b;

  for(int i=0;i<BS_HASH_SIZE;i++) {
    if (aa->bid[i]<bb->bid[i]) return -1;
    if (aa->bid[i]>bb->bid[i]) return 1;
  }
  return 0;
}


int blocktree_pack_node(struct blocktree_node *n)
{
  struct blocktree_node_unpacked *up=&n->unpacked;

  n->block_len=0;

  // Key is that we have to produce a canonical representation, so that we can reliably infer things
  // at each end of a link.
  
  // Sort pointers by hash
  qsort(&up->pointers[0],up->pointer_count,sizeof(struct blocktree_child_pointer),compare_node_child_pointer);
  
  // Sort leaves by hash
  qsort(&up->leaves[0],up->leaf_count,sizeof(struct blocktree_bundle_leaf),compare_bundle_leaf);

  n->block[n->block_len++]=(up->leaf_count<<4)+up->pointer_count;

  for(int i=0;i<up->pointer_count;i++) {
    n->block[n->block_len++]=up->pointers[i].child_bits;
    memcpy(&n->block[n->block_len],up->pointers[i].child_hash,BS_HASH_SIZE);
    n->block_len+=BS_HASH_SIZE;
  }
  for(int i=0;i<up->leaf_count;i++) {
    memcpy(&n->block[n->block_len],up->leaves[i].bid,32);
    n->block_len+=32;
    for(int j=0;j<8;j++)
      n->block[n->block_len++]=up->leaves[i].version>>(56-j*8);
    memcpy(&n->block[n->block_len],up->leaves[i].manifest_hash,BS_HASH_SIZE);
    n->block_len+=BS_HASH_SIZE;
    memcpy(&n->block[n->block_len],up->leaves[i].payload_hash,BS_HASH_SIZE);
    n->block_len+=BS_HASH_SIZE;          
  } 

  return BTOK_SUCCESS;  
}

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

  //  fprintf(stderr,"pointers=%d, leaves=%d\n",n->pointer_count,n->leaf_count);
  //  dump_bytes(stderr,"data block",block,len);
  
  while(ofs<len) {
    if (p<n->pointer_count) {
      // Extract pointer to sub-child
      n->pointers[l].child_bits=block[ofs++];
      memcpy(n->pointers[p].child_hash,&block[ofs],BS_HASH_SIZE); ofs+=BS_HASH_SIZE;
      p++;
    } else if (l<n->leaf_count) {
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
    fprintf(stderr,"ERROR: Corrupt block: Len=%d, but offset only got to %d\n",len,ofs);
    return BTERR_TREE_CORRUPT;
  }

  return BTOK_SUCCESS;
}

char *space_string="                                                                                                                                   ";
char *spaces(int num)
{
  return &space_string[strlen(space_string)-num];
}

char hex_string[1024];
char *to_hex(unsigned char *b,int len)
{
  if (len>500) return "<string too long>";
  for(int i=0;i<len;i++)
    snprintf(&hex_string[i*2],3,"%02X",b[i]);
  hex_string[len*2]=0;
  return hex_string;
}

void blocktree_dump_node(struct blocktree *bt,int depth,unsigned char *hash)
{
  unsigned char node_hash[BS_HASH_SIZE];
  int node_hash_len=BS_HASH_SIZE;
  unsigned char block[BS_MAX_BLOCKSIZE];
  int block_len=BS_MAX_BLOCKSIZE;
  fprintf(stderr,">>> %s %s node hash %s\n",timestamp_str(),spaces(depth*3),to_hex(hash,BS_HASH_SIZE));
  int found_node=0;
  for(int i=0;i<bt->blockstore_count;i++) {
    if (!blockstore_retrieve(bt->blockstores[i],node_hash,node_hash_len,block,&block_len)) {
      found_node=1; break;
    }
  }
  if (!found_node) {
    fprintf(stderr,">>> %s %s BLOCK NOT FOUND IN STORE!\n",
	    timestamp_str(),spaces(depth*3+3));
    return;
  }
  struct blocktree_node node;
  if (!blocktree_unpack_node(block,block_len,&node.unpacked)) {   
    fprintf(stderr,">>> %s %s Node contains %d children and %d leaves\n",
	    timestamp_str(),spaces(depth*3),node.unpacked.leaf_count,node.unpacked.pointer_count);
  } else {
    fprintf(stderr,">>> %s %s BLOCK CORRUPT!\n",
	    timestamp_str(),spaces(depth*3+3));
    
  }
  
  
}

void blocktree_dump(void *blockstore,char *msg)
{
  fprintf(stderr,">>> %s Blocktree contents: %s\n",
	  timestamp_str(),msg);
  struct blocktree *bt=blockstore;
  blocktree_dump_node(blockstore,1,bt->top_hash);
  
  return;
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

int blocktree_bid_depth_bits(int depth,unsigned char *bid)  
{
  int next_bits=0;
  for(int i=0;i<3;i++) {
    next_bits=next_bits<<1;
    if (bid[(depth+i)>>3]&(0x80>>(i&7))) next_bits|=1;
    }
  return next_bits;
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
      memcpy(res.hash,node_hash,node_hash_len);
      return &res;
    }

    // Ok, we have the node, so now take a look in it
    if (blocktree_unpack_node(res.block,res.block_len,&res.unpacked)) {
      // Found it, but can't unpack the node
      res.status=BTERR_TREE_CORRUPT;
      res.depth=bit_offset/3;
      memcpy(res.hash,node_hash,node_hash_len);
      return &res;
    }

    // Ok, we have the node.
    for(i=0;i<res.unpacked.leaf_count;i++) {
      if (!memcmp(res.unpacked.leaves[i].bid,bid,32)) {
	// Found it!
	res.status=BTOK_FOUND;
	res.leaf_num=i;
	res.depth=bit_offset/3;
	memcpy(res.hash,node_hash,node_hash_len);
	return &res;
      }
    }
    // Nope, its not in a leaf node in this block.
    // Find the correct pointer, and follow that, or return
    // BTERR_NOT_IN_TREE if it is definitely not there.
    unsigned char next_bits=blocktree_bid_depth_bits(bit_offset,bid);

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

int blocktree_node_write(void *blocktree,struct blocktree_node *n)
{
  unsigned char i;
  struct blocktree *bt=blocktree;
  if (!bt) return BTERR_TREE_CORRUPT;

  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
  
  /*
    We assume that the node has been re-packed.
    We need to calculate its hash, and write it to the store.
    Then we need to replace the pointer to the old version in 
    the parent block with the new value, and write that back out,
    as well.
    XXX Delete the old versions as we go?

    For embedded systems, we don't really want to have upto 256 / 3 = ~85 node blocks
    hanging around in memory for a worse-case situation.  So we instead iterate through
    the tree from the top each time when doing the update. On non-embedded systems, the
    performance of the block store compared with data transfer speeds should be more than
    ample.  Various caching strategies can be used in that case, anyway, to speed things
    up if required.  For now we will just focus on getting it working.
  */

  unsigned char hash[BS_HASH_SIZE];
  blocktree_hash_data(blocktree,hash,n->block,n->block_len);
  fprintf(stderr,">>> %s Updated block hash is %02x%02x%02x%02x...\n",
	  timestamp_str(),
	  hash[0],hash[1],hash[2],hash[3]);

  // Try each writeable blockstore in turn
  for(i=0;i<bt->blockstore_count;i++)
    if (bt->blockstore_writeable[i]) {
      if (!blockstore_store(bt->blockstores[i],hash,BS_HASH_SIZE,n->block,n->block_len)) {
	break;
      }
    }
  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
  if (i==bt->blockstore_count) {
    return BTERR_NO_SPACE;
  }
  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);

  // Now update the parent node
  while(n->depth) {
    fprintf(stderr,"NOTIMPLEMENTED: Update intermediate nodes in tree.\n");
  }
  memcpy(bt->top_hash,hash,BS_HASH_SIZE);
  
  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
  return BTOK_SUCCESS;
  
}

int blocktree_node_insert_leaf(void *blocktree,struct blocktree_node *n, struct blocktree_bundle_leaf *leaf)
{
  //  int size=n->unpacked.pointer_count*(1+BS_HASH_SIZE)+(n->unpacked.leaf_count+1)*(32+8+BS_HASH_SIZE+BS_HASH_SIZE);
  /* Block would be too big, so write out leaf, plus any other leaves already in the block out
     into separate blocks, then put their pointers in place.
     
     We then have a couple of cases to worry about:
     1. If there was no pointer to the relevant sub-tree, then our life is easy, as we can just add a pointer
     to the newly inserted leaf block.
     2. If there is already a pointer to the relevant sub-tree, then we need to traverse down to work out
     where it goes.
     
     We can simplify this by simply always splitting a block whenever it has > 2 leaves into only pointers,
     so that we should never have a block with pointers AND leaves at the same time.  This loses a bit of
     efficiency of storage (and thus transmission), but it simplifies the algorithm quite a bit.
  */

  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
  if (n->unpacked.pointer_count) {
    // We need to put our leaf onto the right child, by drilling down to the bottom of the tree.
    fprintf(stderr,"UNIMPLEMENTED: Inserting leaf into node with pointers.\n");
    exit(-1);
  }
  if (n->unpacked.leaf_count>1) {
    fprintf(stderr,"UNIMPLEMENTED: Splitting over-full leaf node block.\n");

    blocktree_dump(blocktree,"Before splitting node");
    
    /*
      There are two cases here as well:
      1. The two leaves already in the node should belong to the same sub-tree after the split.
         In this case, the node doesn't need splitting, but rather demotion deeper into the tree,
	 and to be replaced with a pointer to it.
      2. The two leaves already in the node belong in different sub-trees after the split.
         In this case, we need to split the node, and then re-try the insert, which will then put
	 the new leaf into the right place.
    */
    
    if (blocktree_bid_depth_bits(n->depth,n->unpacked.leaves[0].bid)
	!=blocktree_bid_depth_bits(n->depth,n->unpacked.leaves[0].bid))
      {
	// Build the two leaf nodes
	struct  blocktree_node c1,c2;
	bzero(&c1,sizeof(struct blocktree_node));
	bzero(&c2,sizeof(struct blocktree_node));
	c1.unpacked.leaf_count=1;
	memcpy(&c1.unpacked.leaves[0],&n->unpacked.leaves[0],sizeof(struct blocktree_bundle_leaf));
	c2.unpacked.leaf_count=1;
	memcpy(&c2.unpacked.leaves[0],&n->unpacked.leaves[1],sizeof(struct blocktree_bundle_leaf));
	
	// Pack and store them
	c1.depth=0;
	c2.depth=0;
	blocktree_pack_node(&c1);
	blocktree_pack_node(&c2);
	blocktree_node_write(blocktree,&c1);    
	blocktree_node_write(blocktree,&c2);

	unsigned char c1hash[BS_HASH_SIZE];
	unsigned char c2hash[BS_HASH_SIZE];
	blocktree_hash_data(blocktree,c1hash,c1.block,c1.block_len);	
	blocktree_hash_data(blocktree,c2hash,c2.block,c2.block_len);	
	
	
	// Then put the hashes of the leaf nodes into the node, and write it back, too.
	// These should be in canonical order.
	memcpy(&n->unpacked.pointers[0],c1hash,BS_HASH_SIZE);
	memcpy(&n->unpacked.pointers[1],c2hash,BS_HASH_SIZE);
	n->unpacked.pointer_count=2;
	n->unpacked.leaf_count=0;
	blocktree_node_write(blocktree,n);
      }
    else
      {
	// Demote the leaf by making it contain a pointer to itself
	unsigned char hash[BS_HASH_SIZE];
	n->unpacked.leaf_count=0;
	n->unpacked.pointer_count=1;
	blocktree_pack_node(n);
	blocktree_hash_data(blocktree,hash,n->block,n->block_len);	
	memcpy(&n->unpacked.pointers[0],hash,BS_HASH_SIZE);
      }

    // Now in either case we have a node containing either <2 leaves or some pointers.
    blocktree_dump(blocktree,"After splitting node");
    
    exit(-1);
  }

  // Node is currently empty, or contains only one leaf, so we can just stuff it in
  memcpy(&n->unpacked.leaves[n->unpacked.leaf_count],leaf,sizeof(struct blocktree_bundle_leaf));
  n->unpacked.leaf_count++;

  
  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
  return BTOK_SUCCESS;
  
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
  
  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
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

  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
  switch(n->status) {
  case BTERR_NOT_IN_TREE:
    blocktree_dump(blocktree,"Before insert");
    // Its not in the tree, so we can just add it into this node,
    // possibly causing this node to split.
    blocktree_node_insert_leaf(blocktree,n,&leaf);
    // After it has been split, the result should pack fine. If not, we have a terrible bug
    if (blocktree_pack_node(n)) return BTERR_TREE_CORRUPT;
    // Finally, write it back, perculating the change back up to the top of the tree
    blocktree_node_write(blocktree,n);
    blocktree_dump(blocktree,"After insert");
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
      blocktree_pack_node(n);
      blocktree_node_write(blocktree,n);
    } else {
      // We are trying to insert an older version, so nothing to do. We keep
      // the newer version.
    }
    break;
  case BTERR_TREE_CORRUPT: 
  case BTERR_INTERMEDIATE_MISSING:
  default:
    fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
      return n->status;      
  }
  
  // Completed successfully
  fprintf(stderr,"CHECKPOINT:%s:%d:%s()\n",__FILE__,__LINE__,__FUNCTION__);
  return BTOK_SUCCESS;
}
