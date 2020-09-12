#define BT_MAX_SALT_LEN 1024

#define BTOK_FOUND 0
#define BTOK_SUCCESS 0
#define BTERR_NOT_IN_TREE 1
#define BTERR_INTERMEDIATE_MISSING 2
#define BTERR_TREE_CORRUPT 3
#define BTERR_NO_SPACE 4

struct blocktree_bundle_leaf {
  unsigned char bid[32];
  unsigned long long version;
  unsigned char manifest_hash[BS_HASH_SIZE];
  unsigned char payload_hash[BS_HASH_SIZE];
};

struct blocktree_child_pointer {
  // As each block can hold 8 children, 3 bits of
  // the BID space are consumed, and we need to indicate
  // what those three bits are
  unsigned char child_bits;
  unsigned char child_hash[BS_HASH_SIZE];
};

struct blocktree_node_unpacked {
  int pointer_count;
  struct blocktree_child_pointer pointers[8];
  struct blocktree_bundle_leaf leaves[2];
  int leaf_count;
};

struct blocktree_node {
  // Status of operation returning this node
  int status;
  // Depth of the returned node in the tree
  int depth;

  // Which leaf entry is indicated?
  int leaf_num;
  
  // The node ID relevant to the response
  // (meaning varies with response code)
  unsigned char node[BS_MAX_HASH_SIZE];

  // The raw block body relevant to the response
  // (meaning varies with response code)
  unsigned char block[BS_MAX_BLOCKSIZE];
  int block_len;
  
  // Unpacked contents of the node
  struct blocktree_node_unpacked unpacked;
  
};



int blocktree_hash_block(unsigned char *salt_in,int salt_len,
			 unsigned char *hash_out,int hash_len,unsigned char *data,int len);
void *blocktree_open(void *blockstore,
		     unsigned char *salt, int salt_len,
		     unsigned char *hash,int hash_len);
int blocktree_insert_bundle(void *blocktree,
			    char *bid_hex,char *version,
			    unsigned char *manifest_encoded,int manifest_encoded_len,
			    unsigned char *body,int body_len);
