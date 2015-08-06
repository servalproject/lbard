struct segment_list {
  unsigned char *data;
  int start_offset;
  int length;
  struct segment_list *prev,*next;
};

struct partial_bundle {
  // Data from the piece headers for keeping track
  char *bid_prefix;
  long long bundle_version;
  
  struct segment_list *manifest_segments;
  int manifest_length;

  struct segment_list *body_segments;
  int body_length;    
};

struct peer_state {
  char *sid_prefix;
  
  unsigned char *last_message;
  time_t last_message_time;
  int last_message_number;

  // BARs we have seen from them.
  int bundle_count;
#define MAX_PEER_BUNDLES 100000
  int bundle_count_alloc;
  char **bid_prefixes;
  long long *versions;

  // Bundles this peer is transferring.
  // The bundle prioritisation algorithm means that the peer may announce pieces
  // of several bundles interspersed, e.g., a large bundle may be temporarily
  // deferred due to the arrival of a smaller bundle, or the arrival of a peer
  // for whom that peer has bundles with the new peer as the recipient.
  // So we need to carry state for some plurality of bundles being announced.
  // Note that we don't (currently) build bundles from announcements by multiple
  // peers, as a small protection against malicious nodes offering fake bundle
  // pieces that will result in the crypto checksums failing at the end.
#define MAX_BUNDLES_IN_FLIGHT 16
  struct partial_bundle partials[MAX_BUNDLES_IN_FLIGHT];  
}; 


struct bundle_record {
  char *service;
  char *bid;
  long long version;
  char *author;
  int originated_here_p;
  int announce_now;
  long long length;
  char *filehash;
  char *sender;
  char *recipient;

  // The last time we announced this bundle in full.
  time_t last_announced_time;
  // The last version of the bundle that we announced.
  long long last_version_of_manifest_announced;
  // The furthest through the file that we have announced during the current
  // attempt at announcing it (which may be interrupted by the presence of bundles
  // with a higher priority).
  long long last_offset_announced;
  // Similarly for the manifest
  long long last_manifest_offset_announced;
};

#define MAX_PEERS 1024
extern struct peer_state *peer_records[MAX_PEERS];
extern int peer_count;

#define MAX_BUNDLES 10000
extern struct bundle_record bundles[MAX_BUNDLES];
extern int bundle_count;

// BAR consists of:
// 8 bytes : BID prefix
// 8 bytes : version
// 4 bytes : recipient prefix
#define BAR_LENGTH (8+8+4)

extern char *bid_of_cached_bundle;
extern long long cached_version;
extern int cached_manifest_len;
extern unsigned char *cached_manifest;
extern int cached_body_len;
extern unsigned char *cached_body;


int saw_piece(char *peer_prefix,char *bid_prefix,long long version,
	      long long piece_offset,int piece_bytes,int is_end_piece,
	      int is_manifest_piece,unsigned char *piece,

	      char *prefix, char *servald_server, char *credential);
int saw_message(unsigned char *msg,int len,char *my_sid,
		char *prefix, char *servald_server,char *credential);
int load_rhizome_db(int timeout,
		    char *prefix, char *serval_server,
		    char *credential, char **token);
int parse_json_line(char *line,char fields[][8192],int num_fields);
int rhizome_update_bundle(unsigned char *manifest_data,int manifest_length,
			  unsigned char *body_data,int body_length,
			  char *servald_server,char *credential);
int prime_bundle_cache(int bundle_number,char *prefix,
		       char *servald_server, char *credential);
int hex_byte_value(char *hexstring);
int find_highest_priority_bundle();
int find_peer_by_prefix(char *peer_prefix);
int clear_partial(struct partial_bundle *p);
int dump_partial(struct partial_bundle *p);
int merge_segments(struct segment_list **s);
int free_peer(struct peer_state *p);
int peer_note_bar(struct peer_state *p,
		  char *bid_prefix,long long version, char *recipient_prefix);
int announce_bundle_piece(int bundle_number,int *offset,int mtu,unsigned char *msg,
			  char *prefix,char *servald_server, char *credential);
int update_my_message(char *my_sid, int mtu,unsigned char *msg_out,
		      char *servald_server,char *credential);
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream);
