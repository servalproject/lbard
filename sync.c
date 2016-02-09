#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/*
Synchronize two sets of keys, which are likely to contain many common values
*/


// Some utility functions and macros

#define LOG(X) puts(X)
#define LOGF(X, ...) printf(X "\n", ##__VA_ARGS__)

static const char *hexdigit_upper = "0123456789ABCDEF";
static char *tohex(char *dstHex, size_t dstStrLen, const uint8_t *srcBinary)
{
  char *p;
  size_t i;
  for (p = dstHex, i = 0; i < dstStrLen; ++i)
    *p++ = (i & 1) ? hexdigit_upper[*srcBinary++ & 0xf] : hexdigit_upper[*srcBinary >> 4];
  *p = '\0';
  return dstHex;
}

// allocate memory and crash on failure.
static void *allocate(size_t length){
  void *ret = malloc(length);
  if (!ret){
    LOG("Memory allocation failed");
    exit(-1);
  }
  bzero(ret, length);
  return ret;
}



// Definitions of what a key is

#define KEY_LEN 8
#define KEY_LEN_BITS (KEY_LEN<<3)

// Note PREFIX_STEP_BITS >1 hasn't been tested yet
#define PREFIX_STEP_BITS 1
#define NODE_CHILDREN (1<<PREFIX_STEP_BITS)
#define INTERESTING_COUNT 16

typedef struct {
  uint8_t min_prefix_len;
  uint8_t prefix_len;
  uint8_t key[KEY_LEN];
}sync_key_t;

#define alloca_tohex(SRC, LEN) tohex(alloca(LEN*2+1), LEN*2+1, SRC) 
#define alloca_sync_key(K) alloca_tohex((K)->key, KEY_LEN)



// definitions for how we track the state of a set of keys

#define NOT_SENT 0
#define SENT 1
#define QUEUED 2
#define DONT_SEND 3

struct node{
  struct node *transmit_next;
  sync_key_t key;
  uint8_t send_state;
  struct node *children[NODE_CHILDREN];
};

struct sync_state{
  char name[32];
  unsigned key_count;
  unsigned sent_root;
  unsigned sent_messages;
  unsigned sent_record_count;
  unsigned received_record_count;
  unsigned received_uninteresting;
  struct node root;
  struct node *transmit_head;
  struct node *transmit_tail;
};



// XOR the source key into the destination key
// the leading prefix_len bits of the source key will be copied, the remaining bits will be XOR'd
static void sync_xor(const sync_key_t *src_key, sync_key_t *dest_key)
{
  unsigned i=0;
  
  assert(dest_key->prefix_len < KEY_LEN_BITS);
  
  // Assign whole prefix bytes
  for(;i<(dest_key->prefix_len>>3);i++)
    dest_key->key[i] = src_key->key[i];
    
  if (dest_key->prefix_len&7){
    // Mix assignment and xor for the byte of overlap
    uint8_t mask = (0xFF00>>(dest_key->prefix_len&7)) & 0xFF;
    dest_key->key[i] = (mask & src_key->key[i]) | (dest_key->key[i] ^ src_key->key[i]);
    i++;
  }
  
  // Xor whole remaining bytes
  for (;i<KEY_LEN;i++)
    dest_key->key[i] ^= src_key->key[i];
}

#define sync_xor_node(N,K) sync_xor((K), &(N)->key)

// return len bits from the key, starting at offset
static uint8_t sync_get_bits(uint8_t offset, uint8_t len, const sync_key_t *key)
{
  assert(len <= 8);
  assert(offset+len <= KEY_LEN_BITS);
  unsigned start_byte = (offset>>3);
  uint16_t context = key->key[start_byte] <<8 | key->key[start_byte+1];
  return (context >> (16 - (offset & 7) - len)) & ((1<<len) -1);
}

// Dump the raw details of the whole tree
/*
static void dump_tree(struct node *node, int indent)
{
  LOGF("%*s - tree node [%u, %u, %s]", 
    indent, "",
    node->key.min_prefix_len, 
    node->key.prefix_len, 
    alloca_sync_key(&node->key));
    
  if (node->key.prefix_len < KEY_LEN_BITS){
    for (unsigned i=0;i<NODE_CHILDREN;i++){
      if (!node->children[i])
	continue;
      dump_tree(node->children[i], indent+1);
    }
    LOGF("%*s - (end)",
      indent, ""
    );
  }
}
*/

// XOR all existing children of *node, into this destination key.
static void xor_children(struct node *node, sync_key_t *dest)
{
  if (node->key.prefix_len == KEY_LEN_BITS){
    sync_xor(&node->key, dest);
  }else{
    for (unsigned i=0;i<NODE_CHILDREN;i++){
      if (node->children[i])
	xor_children(node->children[i], dest);
    }
  }
}


// Add a new key into the state tree, XOR'ing the key into each parent node
static void sync_add_key(struct sync_state *state, const sync_key_t *key)
{
  uint8_t prefix_len = 0;
  struct node *root = &state->root;
  struct node **node = &root;
  uint8_t min_prefix_len = prefix_len;
  state->key_count++;
  while(1){
    uint8_t child_index = sync_get_bits(prefix_len, PREFIX_STEP_BITS, key);
    
    if ((*node)->key.prefix_len == prefix_len){
      sync_xor_node((*node), key);
      if ((*node)->send_state == SENT)
	(*node)->send_state = NOT_SENT;
      prefix_len += PREFIX_STEP_BITS;
      min_prefix_len = prefix_len;
      node = &(*node)->children[child_index];
      if (!*node){
	// create final leaf node
	*node = allocate(sizeof(struct node));
	(*node)->key = *key;
	(*node)->key.min_prefix_len = min_prefix_len;
	(*node)->key.prefix_len = KEY_LEN_BITS;
	return;
      }
      continue;
    }
    
    // this node represents a range of prefix bits
    uint8_t node_child_index = sync_get_bits(prefix_len, PREFIX_STEP_BITS, &(*node)->key);
    
    // if the prefix matches the key, keep searching.
    if (child_index == node_child_index){
      prefix_len += PREFIX_STEP_BITS;
      continue;
    }
    
    // if there is a mismatch in the range of prefix bits, we need to create a new node to represent the new range.
    struct node *parent = allocate(sizeof(struct node));
    parent->key.min_prefix_len = min_prefix_len;
    parent->key.prefix_len = prefix_len;
    parent->children[node_child_index] = *node;
    
    min_prefix_len = prefix_len + PREFIX_STEP_BITS;
    assert(min_prefix_len <= (*node)->key.prefix_len);
    
    (*node)->key.min_prefix_len = min_prefix_len;
    
    // xor all the existing children of this node, we can't assume the prefix bits are right in the existing node.
    // we might be able to speed this up by using the prefix bits of the passed in key
    xor_children(parent, &parent->key);
    
    *node = parent;
  }
}

// Recursively free the memory used by this tree
static void free_node(struct node *node)
{
  if (!node)
    return;
  for (unsigned i=0;i<NODE_CHILDREN;i++)
    free_node(node->children[i]);
  free(node);
}

// clear all memory used by this state
static void sync_clear_keys(struct sync_state *state)
{
  for (unsigned i=0;i<NODE_CHILDREN;i++)
    free_node(state->root.children[i]);
  bzero(state, sizeof (struct sync_state));
}

// prepare a network packet buffer, with as many queued outgoing messages that we can fit
static size_t sync_build_message(struct sync_state *state, uint8_t *buff, size_t len)
{
  size_t offset=0;
  state->sent_messages++;
  
  while(state->transmit_head && offset + sizeof(sync_key_t)<=len){
    if (state->transmit_head->send_state == QUEUED){
      bcopy(&state->transmit_head->key, &buff[offset], sizeof(sync_key_t));
      offset+=sizeof(sync_key_t);
      state->sent_record_count++;
      state->transmit_head->send_state = SENT;
    }else if(state->transmit_head->send_state == QUEUED)
      state->transmit_head->send_state = DONT_SEND;
    state->transmit_head = state->transmit_head->transmit_next;
  }
  
  if (!state->transmit_head)
    state->transmit_tail = NULL;
  
  // If we don't have anything else to send, always send our root tree node
  if(offset + sizeof(sync_key_t)<=len && offset==0){
    state->sent_root++;
    bcopy(&state->root.key, &buff[offset], sizeof(sync_key_t));
    offset+=sizeof(sync_key_t);
    state->sent_record_count++;
  }
  
  return offset;
}

// Add a tree node into our transmission queue
// the node can be added to the head or tail of the list.
static void queue_node(struct sync_state *state, struct node *node, uint8_t head)
{
  if (node->send_state!=NOT_SENT)
    return;
    
  node->send_state = QUEUED;
  
  if (!state->transmit_head){
    state->transmit_head = state->transmit_tail = node;
    node->transmit_next = NULL;
  }else if (head){
    // push onto head
    node->transmit_next = state->transmit_head;
    state->transmit_head = node;
  }else{
    node->transmit_next = NULL;
    state->transmit_tail->transmit_next = node;
    state->transmit_tail = node;
  }
}

// traverse the children of this node, and add them all to the transmit queue
// optionally ignoring a single child of this node.
static void queue_leaf_nodes(struct sync_state *state, struct node *node, unsigned except)
{
  if (node->key.prefix_len == KEY_LEN_BITS){
    queue_node(state, node, 1);
  }else{
    for (unsigned i=0;i<NODE_CHILDREN;i++){
      if (node->children[i] && i!=except)
	queue_leaf_nodes(state, node->children[i], NODE_CHILDREN);
    }
  }
}

#define MIN_VAL(X,Y) ((X)<(Y)?(X):(Y))
#define MAX_VAL(X,Y) ((X)<(Y)?(Y):(X))

// Compare two keys, returning zero if they represent the same set of leaf nodes.
static int cmp_key(const sync_key_t *first, const sync_key_t *second)
{
  uint8_t common_prefix_len = MIN_VAL(first->prefix_len, second->prefix_len);
  uint8_t first_xor_begin = (first->prefix_len == KEY_LEN_BITS)?first->min_prefix_len:first->prefix_len;
  uint8_t second_xor_begin = (second->prefix_len == KEY_LEN_BITS)?second->min_prefix_len:second->prefix_len;
  uint8_t xor_begin_offset = MAX_VAL(first_xor_begin, second_xor_begin);
  int ret=0;
  
  // TODO at least we can compare before common_prefix_len and after xor_begin_offset
  // But we aren't comparing the bits in the middle
      
  if (common_prefix_len < xor_begin_offset){
    if (common_prefix_len>=8 && memcmp(&first->key, &second->key, common_prefix_len>>3)!=0)
      ret = -1;
    else{
      uint8_t xor_begin_byte = (xor_begin_offset+7)>>3;
      if (xor_begin_byte < KEY_LEN && memcmp(&first->key[xor_begin_byte], &second->key[xor_begin_byte], KEY_LEN - xor_begin_byte)!=0)
	ret = -1;
    }
  }else{
    ret = memcmp(&first->key, &second->key, KEY_LEN);
  }
  return ret;
}

// Proccess one incoming tree record.
static void recv_key(struct sync_state *state, const sync_key_t *key)
{
  state->received_record_count++;
  
  /* Possible outcomes;
    key is an exact match for part of our tree
      Yay, nothing to do.
    
    key->prefix_len == KEY_LEN_BITS && we don't have this node
      Woohoo, we discovered something we didn't know before!
    
    they are missing sibling nodes between their min_prefix_len and prefix_len
      queue all the sibling leaf nodes!
      
    our node doesn't match
      XOR our node against theirs
      search our tree for a single leaf node that matches this result
      if found;
	queue this leaf node for transmission
      else
	drill down our tree while our node has only one child? TODO our tree nodes should never have one child
	queue (N-1 of?) this node's children for transmission
  */
  
  struct node *node = &state->root;
  uint8_t prefix_len = 0;
  
  while(1){
    // Nothing to do if we have a node that matches.
    if (cmp_key(key, &node->key)==0){
      state->received_uninteresting++;
      // if we queued this node, there's no point now.
      if (node->send_state == QUEUED)
	node->send_state = DONT_SEND;
      return;
    }
    
    // once we've looked at all of the prefix_len bits of the incoming key
    // we need to stop
    if (key->prefix_len <= prefix_len){
      if (node->key.prefix_len > key->prefix_len){
	// reply with our matching node
	queue_node(state, node, 1);
      }else{
	// compare their node to our tree, test if we can easily detect a part of our tree they don't know
	
	// work out the difference between their node and ours
	sync_key_t test_key = *key;
	sync_xor(&node->key, &test_key);
	
	// if we can explain the difference based on a matching node, queue all leaf nodes
	struct node *test_node = node;
	uint8_t test_prefix = prefix_len;
	while(test_node) {
	  if (cmp_key(&test_key, &test_node->key)==0){
	    // This peer doesn't know any of the children of this node
	    queue_leaf_nodes(state, test_node, NODE_CHILDREN);
	    return;
	  }
	  if (test_node->key.prefix_len == KEY_LEN_BITS)
	    break;
	  uint8_t child_index = sync_get_bits(test_prefix, PREFIX_STEP_BITS, &test_key);
	  if (test_prefix<test_node->key.prefix_len){
	    // TODO optimise this case by comparing all possible prefix bits in one hit
	    uint8_t node_index = sync_get_bits(test_prefix, PREFIX_STEP_BITS, &test_node->key);
	    if (node_index != child_index)
	      break; // no match
	  }else{
	    test_node = test_node->children[child_index];
	  }
	  test_prefix+=PREFIX_STEP_BITS;
	}
	
	// queue the transmission of all child nodes of this node
	for (unsigned i=0;i<NODE_CHILDREN;i++){
	  if (node->children[i])
	    queue_node(state, node->children[i], 0);
	}
      }
      return;
    }
    
    // which branch of the tree should we look at next
    uint8_t key_index = sync_get_bits(prefix_len, PREFIX_STEP_BITS, key);
  
    // if our node represents a large range of the keyspace, find the first prefix bit that differs
    while (prefix_len < node->key.prefix_len && prefix_len < key->prefix_len){
      // check the next step bits
      uint8_t existing_index = sync_get_bits(prefix_len, PREFIX_STEP_BITS, &node->key);
      if (key_index != existing_index){
	// If the prefix of our node differs from theirs, they don't have any of these keys
	// send them all
	if (prefix_len >= key->min_prefix_len){
	  queue_leaf_nodes(state, node, NODE_CHILDREN);
	    
	  if (key->prefix_len != KEY_LEN_BITS)
	    // and after they have added all these missing keys, they need to know 
	    // this summary node so they can be reminded to send this key or it's children again.
	    queue_node(state, node, 0);
	}
	
	if (key->prefix_len == KEY_LEN_BITS){
	  // They told us about a single key we didn't know.
	  sync_add_key(state, key);
	}
	return;
      }
      prefix_len += PREFIX_STEP_BITS;
      key_index = sync_get_bits(prefix_len, PREFIX_STEP_BITS, key);
    }
    
    if (key->prefix_len <= prefix_len)
      continue;
    
    assert(prefix_len == node->key.prefix_len);
    
    if (key->min_prefix_len <= node->key.prefix_len){
      // send all keys to the other party, except for the child @key_index
      // they don't have any of these siblings
      queue_leaf_nodes(state, node, key_index);
    }
    
    // look at the next node in our graph
    if (!node->children[key_index]){
      // we know nothing about this key
      if (key->prefix_len == KEY_LEN_BITS){
	//Yay, they told us something we didn't know.
	sync_add_key(state, key);
      }else{
	// hopefully the other party will tell us something,
	// and we won't get stuck in a loop talking about the same node.
	queue_node(state, node, 0);
      }
      return;
    }
    
    node = node->children[key_index];
    prefix_len += PREFIX_STEP_BITS;
  }
}

// Process all incoming messages from this packet buffer
static void sync_recv_message(struct sync_state *state, uint8_t *buff, size_t len)
{
  size_t offset=0;
  while(offset + sizeof(sync_key_t)<=len){
    sync_key_t *key = (sync_key_t *)&buff[offset];
    recv_key(state, key);
    offset+=sizeof(sync_key_t);
  }
}


// compare two tree's, logging any differences.
// returns 0 if the tree's are the same
static int cmp_trees(
  const struct sync_state *peer_left, 
  const struct sync_state *peer_right, 
  const struct node *left, 
  const struct node *right)
{
  int ret=0;
  if (right && (left==NULL || left->key.prefix_len != right->key.prefix_len)){
    LOGF("(%s) has [%u, %u, %s]",
      peer_right->name, 
      right->key.min_prefix_len, 
      right->key.prefix_len, 
      alloca_sync_key(&right->key)
    );
    ret=1;
    for (unsigned i=0;i<NODE_CHILDREN;i++)
      cmp_trees(peer_left, peer_right, NULL, right->children[i]);
  }
  
  if (left && (right==NULL || left->key.prefix_len != right->key.prefix_len)){
    LOGF("(%s) has [%u, %u, %s]",
      peer_left->name, 
      left->key.min_prefix_len, 
      left->key.prefix_len, 
      alloca_sync_key(&left->key)
    );
    ret=1;
    for (unsigned i=0;i<NODE_CHILDREN;i++)
      cmp_trees(peer_left, peer_right, left->children[i], NULL);
  }
  
  if (left==NULL || right == NULL || left->key.prefix_len != right->key.prefix_len)
    return ret;
  
  if (cmp_key(&left->key, &right->key)!=0){
    LOGF("Keys differ [%u, %u, %s] vs [%u, %u, %s]", 
      left->key.min_prefix_len, 
      left->key.prefix_len, 
      alloca_sync_key(&left->key),
      right->key.min_prefix_len, 
      right->key.prefix_len, 
      alloca_sync_key(&right->key)
    );
    ret = 1;
  }
  for (unsigned i=0;i<NODE_CHILDREN;i++)
    ret |= cmp_trees(peer_left, peer_right, left->children[i], right->children[i]);
    
  return ret;
}


// test this synchronisation protocol by;
// generating sets of keys
// swapping messages
// stopping when all nodes agree on the set of keys
// logging packet statistics
int main(int argc, char **argv)
{
  // setup peer state
  unsigned peer_count=argc > 2 ? argc-2 : 2;
  struct sync_state peers[peer_count];
  bzero(&peers, sizeof peers);
  
  unsigned i, j;
  for (i=0;i<peer_count;i++)
    snprintf(peers[i].name, sizeof peers[i].name, "Peer %u", i);
  
  LOG("--- Adding keys ---");
  {
    int fdRand = open("/dev/urandom",O_RDONLY);
    assert(fdRand!=-1);
    
    unsigned common=100;
    if (argc>=2)
      common = atoi(argv[1]);
    
    LOGF("Generating %u common keys", common);
    for (i=0; i<common; i++){
      sync_key_t key;
      assert(read(fdRand, key.key, KEY_LEN)==KEY_LEN);
      for (j=0;j<peer_count;j++)
	sync_add_key(&peers[j], &key);
    }
    
    for (i=0; i<peer_count; i++){
      unsigned unique=10;
      if (argc>i+2)
	unique = atoi(argv[i+2]);
      LOGF("Generating %u unique keys for %s", unique, peers[i].name);
      sync_key_t key;
      for (j=0;j<unique;j++){
	assert(read(fdRand, key.key, KEY_LEN)==KEY_LEN);
	sync_add_key(&peers[i], &key);
      }
    }

    close(fdRand);
  }
  // debug, dump the tree structure
  LOG("--- BEFORE ---");
  for (i=0; i<peer_count; i++){
    LOGF("%d Keys known by %s", 
      peers[i].key_count, peers[i].name);
    //dump_tree(&peer_left.root,0);
  }

  LOG("--- SYNCING ---");
  // send messages to discover missing keys
  unsigned sent=0;
  uint8_t trees_match=0;
  
  while(trees_match==0){
    for (i=0;i<peer_count && trees_match==0;i++){
      // transmit one message from peer i to all other peers
      trees_match = 1;
      uint8_t packet_buff[200];
      size_t len = sync_build_message(&peers[i], packet_buff, sizeof(packet_buff));
      sent++;
      for (j=0;j<peer_count;j++){
	if (i!=j){
	  sync_recv_message(&peers[j], packet_buff, len);
	  if (cmp_key(&peers[i].root.key, &peers[j].root.key)!=0)
	    trees_match = 0;
	}
      }
    }
  }
  
  LOGF("Test ended after transmitting %u packets", sent);
  
  for (i=0;i<peer_count;i++){
    LOGF("%s; Keys %u, sent %u, sent root %u, messages %u, received %u, uninteresting %u", 
      peers[i].name, 
      peers[i].key_count, 
      peers[i].sent_messages,
      peers[i].sent_root,
      peers[i].sent_record_count, 
      peers[i].received_record_count,
      peers[i].received_uninteresting);
      
    sync_clear_keys(&peers[i]);
  }
  
  return 0;
}