/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2016 Flinders University
Copyright (C) 2016 Serval Project, Inc.

This program monitors a local Rhizome database and attempts
to synchronise it over low-bandwidth declarative transports, 
such as bluetooth name or wifi-direct service information
messages.  It is intended to give a high priority to MeshMS
converations among nearby nodes.

The design is fully asynchronous, so a call to the update_my_message()
function from time to time should be all that is required.


This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/


// Length of sync keys in bytes. Short keys makes the protocol much more efficient,
// but increases the risk of colissions, which we manage by using a salt so that
// colissions are only transient.
#define SYNC_KEY_LEN 8
#define KEY_LEN SYNC_KEY_LEN
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
  uint8_t sent_count;
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
  unsigned progress;
  struct node root;
  struct node *transmit_ptr;
};

static unsigned max_retries = 1;


// Length of salt used to calculate sync keys for bundle tree
// (salt is changed to protect against accidental or intentional sync key colissions)
#define SYNC_SALT_LEN 8

int bundle_calculate_tree_key(uint8_t bundle_tree_key[SYNC_KEY_LEN],
			      uint8_t sync_tree_salt[SYNC_SALT_LEN],
			      char *bid,
			      long long version,
			      long long length,
			      char *filehash);


// The following section is from sha1.c in this repository, and is in the public domain.

/* header */

#define HASH_LENGTH 20
#define BLOCK_LENGTH 64

typedef struct sha1nfo {
        uint32_t buffer[BLOCK_LENGTH/4];
        uint32_t state[HASH_LENGTH/4];
        uint32_t byteCount;
        uint8_t bufferOffset;
        uint8_t keyBuffer[BLOCK_LENGTH];
        uint8_t innerHash[HASH_LENGTH];
} sha1nfo;

/* public API - prototypes - TODO: doxygen*/

/**
 */
void sha1_init(sha1nfo *s);
/**
 */
void sha1_writebyte(sha1nfo *s, uint8_t data);
/**
 */
void sha1_write(sha1nfo *s, const char *data, size_t len);
/**
 */
uint8_t* sha1_result(sha1nfo *s);
/**
 */
void sha1_initHmac(sha1nfo *s, const uint8_t* key, int keyLength);
/**
 */
uint8_t* sha1_resultHmac(sha1nfo *s);

// Add a new key into the state tree, XOR'ing the key into each parent node
void sync_add_key(struct sync_state *state, const sync_key_t *key);
// clear all memory used by this state
void sync_clear_keys(struct sync_state *state);
// prepare a network packet buffer, with as many queued outgoing messages that we can fit
size_t sync_build_message(struct sync_state *state, uint8_t *buff, size_t len);
// Process all incoming messages from this packet buffer
int sync_recv_message(struct sync_state *state, uint8_t *buff, size_t len);

int sync_by_tree_stuff_packet(int *offset,int mtu, unsigned char *msg_out,
			      char *sid_prefix_bin,
			      char *servald_server,char *credential);
void sync_tree_suspect_peer_does_not_have_this_key(struct sync_state *state,
						   uint8_t key[SYNC_KEY_LEN]);
int sync_tell_peer_we_have_this_bundle(int peer, int bundle);
int sync_schedule_progress_report(int peer, int partial);
int sync_tree_prepare_tree(int peer);
int key_exists(const struct sync_state *state, const sync_key_t *key);
