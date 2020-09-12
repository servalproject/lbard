/*
  Import Rhizome bundles into a block store.

*/

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

#include "sync.h"
#include "lbard.h"
#include "blockstore.h"
#include "blocktree.h"

// Satisfy dependencies of code pulled in from LBARD
#include "radio_type.h"
int debug_insert=1;
char *my_sid_hex="NOT SET";
radio_type radio_types[]={
			  {-1,NULL,NULL,NULL,NULL,NULL,NULL,NULL,-1}
};
long long last_servald_contact=0;



void usage(void)
{
  printf("usage: rhizomeimport <servald server> <credential> <blockstore descriptor>\n"
	 "\n"
	 "   servald server        - IP:port of Servald server\n"
	 "   credential            - user:pass for servald Rhizome RESTful API\n"
	 "   blockstore descriptor - Argument passed when opening/creating block store.\n"
	 );
  exit(-1);
}

char *servald_server=NULL;
char *credential=NULL;
void *blockstore=NULL;
void *blocktree=NULL;

int main(int argc,char **argv)
{
  if (argc!=4) {
    usage();
  }

  servald_server=argv[1];
  credential=argv[2];
  char *blockstore_descriptor=argv[3];
  char token[1024]="";

  blockstore = blockstore_create(64*1024*1024,64*1024*1024,blockstore_descriptor);
  if (!blockstore) {
    fprintf(stderr,"ERROR: Failed to create/open block store.\n");
    exit(-1);
  }

  // Make sure the empty block is stored
  unsigned char zeroes[200];
  unsigned char hash[BS_MAX_HASH_SIZE];
  int hash_len=BS_HASH_SIZE;
  blocktree_hash_block(zeroes,0, // no salt
		       hash,hash_len,zeroes,0);
  blockstore_store(blockstore,hash,hash_len,zeroes,0);
  
  // Start with blocktree pointing to an empty tree.  
  blocktree = blocktree_open(blockstore,hash,hash_len);

  // Re-use normal LBARD rhizome async fetch.
  // It doesn't tell us explicitly when done, so we have to infer a bit
  errno=EAGAIN;
  while (errno==EAGAIN) {
    if (load_rhizome_db_async(servald_server,credential,token)) {
      break;
    }
  }
}

char *bid_of_cached_bundle=NULL;
long long cached_version=0;
int cached_manifest_len=0;
unsigned char *cached_manifest=NULL;
int cached_manifest_encoded_len=0;
unsigned char *cached_manifest_encoded=NULL;
int cached_body_len=0;
unsigned char *cached_body=NULL;


int fetch_bundle(char *bid_hex,char *version,
		 char *servald_server, char *credential)
{
  // Cache is invalid - release
  if (bid_of_cached_bundle) {
    free(bid_of_cached_bundle); bid_of_cached_bundle=NULL;
    free(cached_manifest); cached_manifest=NULL;
    free(cached_manifest_encoded); cached_manifest_encoded=NULL;
    free(cached_body); cached_body=NULL;
  }
  
  // Load bundle into cache
  char path[8192];
  char filename[1024];
  
  snprintf(path,8192,"/restful/rhizome/%s.rhm",
	   bid_hex);
  
  long long t1=gettime_ms();
  
  snprintf(filename,1024,"temp.manifest");
  
  unlink(filename);
  FILE *f=fopen(filename,"w");
  if (!f) {
    fprintf(stderr,"could not open output file '%s'.\n",filename);
    perror("fopen");
    return -1;
  }
  int result_code=http_get_simple(servald_server,
				  credential,path,f,5000,NULL,0);
  fclose(f);
  if(result_code!=200) {
    fprintf(stderr,"http request failed (%d). URLPATH:%s\n",result_code,path);
    return -1;
  }
  long long t2=gettime_ms();
  f=fopen(filename,"r");
  if (!f) {
    fprintf(stderr,"ERROR: Could not open '%s' to read bundle manifest in prime_bundle_cache() call for bundle %s\n",
	    filename,bid_hex);
    perror("fopen");
    return -1;
  }
  if (cached_manifest) free(cached_manifest);
  cached_manifest=malloc(8192);
  assert(cached_manifest);
  cached_manifest_len=fread(cached_manifest,1,8192,f);
  cached_manifest=realloc(cached_manifest,cached_manifest_len);
  assert(cached_manifest);
  fclose(f);
  unlink(filename);
  if (1) fprintf(stderr,"  manifest is %d bytes long.\n",cached_manifest_len);
  
  // Reject over-length manifests
  if (cached_manifest_len>1024) return -1;
  
  // Generate binary encoded manifest from plain text version
  if (cached_manifest_encoded) free(cached_manifest_encoded);
  cached_manifest_encoded=malloc(1024);
  assert(cached_manifest_encoded);
  cached_manifest_encoded_len=0;
  if (manifest_text_to_binary(cached_manifest,cached_manifest_len,
			      cached_manifest_encoded,
			      &cached_manifest_encoded_len)) {
    // Failed to binary encode manifest, so just copy it
    bcopy(cached_manifest,cached_manifest_encoded,cached_manifest_len);
    cached_manifest_encoded_len = cached_manifest_len;	
  }
  fflush(stderr);    
  dump_bytes(stdout,"Compressed manifest for sending",cached_manifest_encoded,cached_manifest_encoded_len);
  fflush(stdout);
  
  snprintf(path,8192,"/restful/rhizome/%s/raw.bin",
	   bid_hex);
  snprintf(filename,1024,"temp.raw");
  unlink(filename);
  f=fopen(filename,"w");
  if (!f) {
    fprintf(stderr,"could not open output file '%s'.\n",filename);
    perror("fopen");
    return -1;
  }
  result_code=http_get_simple(servald_server,
			      credential,path,f,5000,NULL,0);
  fclose(f); f=NULL;
  if(result_code!=200) {
    fprintf(stderr,"http request failed (%d). URLPATH:%s\n",result_code,path);
    return -1;
  }
  long long t3=gettime_ms();
  
  if (0)
    fprintf(stderr,"  HTTP pre-fetching of next bundle to send took %lldms + %lldms\n",
	    t2-t1,t3-t2);
  
  // XXX - This transport only allows bundles upto 5MB!
  // (and that is probably pushing it a bit for a mesh extender with only 32MB RAM
  // for everything!)
  f=fopen(filename,"r");
  if (!f) {
    fprintf(stderr,"could read file '%s'.\n",filename);
    perror("fopen");
    return -1;
  }
  if (cached_body) free(cached_body);
  cached_body=malloc(5*1024*1024);
  assert(cached_body);
  // XXX - Should check that we read all the bytes
  cached_body_len=fread(cached_body,1,5*1024*1024,f);    
  cached_body=realloc(cached_body,cached_body_len);
  if (cached_body_len) assert(cached_body); else fprintf(stderr,"WARNING:Body len = 0 bytes!\n");
  fclose(f);
  unlink(filename);
  if (1)
    fprintf(stderr,"  body is %d bytes long. result_code=%d\n",
	    cached_body_len,result_code);
  
  bid_of_cached_bundle=strdup(bid_hex);
  
  cached_version=strtoll(version,NULL,10);
  
  if (1)
    fprintf(stderr,"Cached manifest and body for %s\n",
	    bid_hex);
  
  return 0;
}


int register_bundle(char *service,
                    char *bid,
                    char *version,
                    char *author,
                    char *originated_here,
                    long long length,
                    char *filehash,
                    char *sender,
                    char *recipient,
                    char *name)
{

  fetch_bundle(bid,version,servald_server,credential);

#if 0
  blockify_bundle(&blocktree,
		  blockstore,
		  bid,version,
		  cached_manifest_encoded,cached_manifest_encoded_len,
		  cached_body,cached_body_len);
#endif  
  
  return 0;
}
