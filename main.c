/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2015 Serval Project Inc.

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

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>

struct peer_state {
  char *sid;
  
  unsigned char *last_message;
  time_t last_message_time;

  int bundle_count;
  char **bids;
  long long *versions;
};

#define MAX_PEERS 1024
struct peer_state *peer_records[MAX_PEERS];
int peer_count=0;

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

int parse_json_line(char *line,char fields[][8192],int num_fields)
{
  int field_count=0;
  int offset=0;
  if (line[offset]!='[') return -1; else offset++;

  while(line[offset]&&line[offset]!=']') {
    if (field_count>=num_fields) return -2;
    if (line[offset]=='"') {
      // quoted field
      int j=0,i;
      for(i=offset+1;(line[i]!='"')&&(i<8191);i++)
	fields[field_count][j++]=line[i];
      fields[field_count++][j]=0;
      offset=i+1;
    } else {
      // naked field
      int j=0,i;
      for(i=offset;(line[i]!=',')&&(line[i]!=']')&&(i<8191);i++)
	fields[field_count][j++]=line[i];
      fields[field_count++][j]=0;
      if (offset==i) return -4;
      offset=i;
    }
    if (line[offset]&&(line[offset]!=',')&&(line[offset]!=']')) return -3;
    if (line[offset]==',') offset++;
  }
  
  return field_count;
}

struct bundle_record {
  char *service;
  char *bid;
  long long version;
  char *author;
  int originated_here_p;
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
};

#define MAX_BUNDLES 10000
struct bundle_record bundles[MAX_BUNDLES];
int bundle_count=0;

int register_bundle(char *service,
		    char *bid,
		    char *version,
		    char *author,
		    char *originated_here,
		    long long length,
		    char *filehash,
		    char *sender,
		    char *recipient)
{
  int i;
  // XXX - Linear search through bundles!
  // Use a hash table or something so that it doesn't cost O(n^2) with number
  // of bundles.
  int bundle_number=bundle_count;
  for(i=0;i<bundle_count;i++) {
    if (!strcmp(bundles[i].bid,bid)) {
      // Updating an existing bundle
      bundle_number=i; break;
    }
  }

  if (bundle_number>=MAX_BUNDLES) return -1;
  
  if (bundle_number<bundle_count) {
    // Replace old bundle values
    free(bundles[bundle_number].service);
    free(bundles[bundle_number].author);
    free(bundles[bundle_number].filehash);
    free(bundles[bundle_number].sender);
    free(bundles[bundle_number].recipient);
  } else {    
    // New bundle
    bundles[bundle_number].bid=strdup(bid);
    // Never announced
    bundles[bundle_number].last_offset_announced=0;
    bundles[bundle_number].last_version_of_manifest_announced=0;
    bundles[bundle_number].last_announced_time=0;
    bundle_count++;
  }
  
  bundles[bundle_number].service=strdup(service);
  bundles[bundle_number].version=strtoll(version,NULL,10);
  bundles[bundle_number].author=strdup(author);
  bundles[bundle_number].originated_here_p=atoi(originated_here);
  bundles[bundle_number].length=length;
  bundles[bundle_number].filehash=strdup(filehash);
  bundles[bundle_number].sender=strdup(sender);
  bundles[bundle_number].recipient=strdup(recipient);
  
  return 0;
}

int find_highest_priority_bundle()
{
  int i;
  int highest_bundle_priority=-1;
  int highest_priority_bundle=-1;

  int this_bundle_priority;
  for(i=0;i<bundle_count;i++) {

    this_bundle_priority=0;

    // Bigger values in this list means that factor is more important in selecting
    // which bundle to announce next.
#define BUNDLE_PRIORITY_SENT_LESS_RECENTLY   0x00000080
#define BUNDLE_PRIORITY_FILE_SIZE_SMALLER    0x00000100
#define BUNDLE_PRIORITY_RECIPIENT_IS_A_PEER  0x00000200
#define BUNDLE_PRIORITY_IS_MESHMS            0x00000400
    
    if (highest_priority_bundle>=0) {
      if (bundles[i].last_announced_time
	  <bundles[highest_priority_bundle].last_announced_time)
	this_bundle_priority|=BUNDLE_PRIORITY_SENT_LESS_RECENTLY;
      if (bundles[i].length
	  <bundles[highest_priority_bundle].length)
	this_bundle_priority|=BUNDLE_PRIORITY_FILE_SIZE_SMALLER;
    }

    if ((!strcasecmp("MeshMS1",bundles[i].service))
	||(!strcasecmp("MeshMS2",bundles[i].service))) {
      this_bundle_priority|=BUNDLE_PRIORITY_IS_MESHMS;
    }
    
    // Is bundle addressed to a peer?
    int j;
    for(j=0;j<peer_count;j++) {
      if (!strcmp(bundles[i].recipient,peer_records[j]->sid)) {
	// Bundle is addressed to a peer.
	// Increase priority if we do not have positive confirmation that peer
	// has this version of this bundle.
	this_bundle_priority|=BUNDLE_PRIORITY_RECIPIENT_IS_A_PEER;

	int k;
	for(k=0;k<peer_records[j]->bundle_count;k++) {
	  if (!strcmp(peer_records[j]->bids[k],bundles[i].bid)) {
	    // Peer knows about this bundle, but which version?
	    if (peer_records[j]->versions[k]<bundles[i].version) {
	      // They only know about an older version.
	      // XXX Advance bundle announced offset to last known offset for
	      // journal bundles (MeshMS1 & MeshMS2 types, and possibly others)
	    } else {
	      // The peer has this version (or possibly a newer version!), so there
	      // is no point us announcing it.
	      this_bundle_priority&=~BUNDLE_PRIORITY_RECIPIENT_IS_A_PEER;
	    }
	  }
	}

	// We found who this bundle was addressed to, so there is no point looking
	// further.
	break;
      }
    }

    // Indicate this bundle as highes priority, unless we have found another one that
    // is higher priority.
    // Replace if priority is equal, so that newer bundles take priorty over older
    // ones.
    if (this_bundle_priority>=highest_priority_bundle) {
      highest_bundle_priority=this_bundle_priority;
      highest_priority_bundle=i;
    }
  }
  
  return highest_priority_bundle;
}

int load_rhizome_db(char *servald_server,char *credential)
{
  CURL *curl;
  CURLcode result_code;
  curl=curl_easy_init();
  if (!curl) return -1;
  char url[8192];
  snprintf(url,8192,"http://%s/restful/rhizome/bundlelist.json",
	   servald_server);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  FILE *f=fopen("bundle.list","w");
  if (!f) {
    curl_easy_cleanup(curl);
    fprintf(stderr,"could not open output file.\n");
    return -1;
  }
  curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  curl_easy_setopt(curl, CURLOPT_USERPWD, credential);  
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  result_code=curl_easy_perform(curl);
  fclose(f);
  if(result_code!=CURLE_OK) {
    curl_easy_cleanup(curl);
    fprintf(stderr,"curl request failed. URL:%s\n",url);
    fprintf(stderr,"libcurl: %s\n",curl_easy_strerror(result_code));
    return -1;
  }

  curl_easy_cleanup(curl);
  fprintf(stderr,"Read bundle list.\n");

  // Now read database into memory.
  f=fopen("bundle.list","r");
  if (!f) return -1;
  char line[8192];
  int count=0;

  char fields[14][8192];
  
  line[0]=0; fgets(line,8192,f);
  while(line[0]) {
    int n=parse_json_line(line,fields,14);
    if (n==14) {
      if (strcmp(fields[0],"null")) {
	// We have a token that will allow us to ask for only newer bundles in a
	// future call. Remember it and use it.
      }
      
      // Now we have the fields, so register the bundles into our internal list.
      register_bundle(fields[2] // service (file/meshms1/meshsm2)
		      ,fields[3] // bundle id (BID)
		      ,fields[4] // version
		      ,fields[7] // author
		      ,fields[8] // originated here
		      ,strtoll(fields[9],NULL,10) // size of data/file
		      ,fields[10] // file hash
		      ,fields[11] // sender
		      ,fields[12] // recipient
		      );
      count++;
    }

    line[0]=0; fgets(line,8192,f);
  }

  fprintf(stderr,"Found %d bundles.\n",count);
  fclose(f);
  
  return 0;
}

int hex_byte_value(char *hexstring)
{
  char hex[3];
  hex[0]=hexstring[0];
  hex[1]=hexstring[1];
  hex[2]=0;
  return strtoll(hex,NULL,16);
}

unsigned char my_sid[32];

int bundle_bar_counter=0;
int append_bar(int bundle_number,int *offset,int mtu,unsigned char *msg_out)
{
  // BAR consists of:
  // 8 bytes : BID prefix
  // 8 bytes : version
  // 4 bytes : recipient prefix
#define BAR_LENGTH (8+8+4)

  for(int i=0;i<8;i++)
    msg_out[(*offset)++]=hex_byte_value(&bundles[bundle_number].bid[i*2]);
  for(int i=0;i<8;i++)
    msg_out[(*offset)++]=(bundles[bundle_number].version>>(i*8))&0xff;
  for(int i=0;i<4;i++)
    msg_out[(*offset)++]=hex_byte_value(&bundles[bundle_number].recipient[i*2]);
  
  return 0;
}


int message_counter=0;
int update_my_message(int mtu,unsigned char *msg_out)
{
  /* There are a few possible options here.
     1. We have no peers. In which case, there is little point doing anything.
        EXCEPT that some people might be able to hear us, even though we can't
	hear them.  So we should walk through a prioritised ordering of some subset
	of bundles, presenting them in turn via the interface.
     2. We have peers, but we have no content addressed to them, that we have not
        already communicated to them.
        In which case, we act as for (1) above.
     3. We have peers, and have bundles addressed to one or more of them, and have
        not yet heard from those peers that they already have those bundles. In which
        case we should walk through presenting those bundles repeatedly until the
        peers acknowledge the receipt of those bundles.

	Thus we need to keep track of the bundles that other peers have, and that have
	been announced to us.

	We also need to take care to announce MeshMS bundles, and especially new and
	updated MeshMS bundles that are addressed to our peers so that MeshMS has
	minimal latency over the transport.  In other words, we don't want to have to
	wait hours for the MeshMS bundle in question to get announced.

	Thus we need some sense of "last announcement time" for bundles so that we can
	prioritise them.  This should be kept with the bundle record.  Then we can
	simply lookup the highest priority bundle, see where we got to in announcing
	it, and announce the next piece of it.

	We should probably also announce a BAR or two, so that peers know if we have
	received bundles that they are currently sending.  Similarly, if we see a BAR
	from a peer for a bundle that they have already received, we should reduce the
	priority of sending that bundle, in particular if the bundle is addressed to
	them, i.e., we have confirmation that the recipient has received the bundle.

	Finally, we should use network coding so that recipients can miss some messages
	without terribly upsetting the whole thing, unless the transport is known to be
	reliable.
  */

  // Build output message

  if (mtu<64) return -1;
  
  // Clear message
  bzero(msg_out,mtu);

  // Put prefix of our SID in first 6 bytes.
  for(int i=0;i<6;i++) msg_out[i]=my_sid[i];
  
  // Put 2-byte message counter
  msg_out[6]=message_counter&0xff;
  msg_out[7]=(message_counter>>8)&0xff;

  int offset=8;
  
  // Put one or more BARs
  bundle_bar_counter++;
  if (bundle_bar_counter>=bundle_count) bundle_bar_counter=0;
  if (bundle_count&&((mtu-offset)>=BAR_LENGTH)) {
    msg_out[offset++]='B'; // indicates a BAR follows
    append_bar(bundle_bar_counter,&offset,mtu,msg_out);
  }

  // Announce a bundle, if any are due.
  int bundle_to_announce=find_highest_priority_bundle();
  fprintf(stderr,"Next bundle to announce is %d\n",bundle_to_announce);

  // Fill up spare space with BARs
  while (bundle_count&&(mtu-offset)>=BAR_LENGTH) {
    bundle_bar_counter++;
    if (bundle_bar_counter>=bundle_count) bundle_bar_counter=0;
    msg_out[offset++]='B'; // indicates a BAR follows
    append_bar(bundle_bar_counter,&offset,mtu,msg_out);
  }
  fprintf(stderr,"bundle_bar_counter=%d\n",bundle_bar_counter);
    
  // Increment message counter
  message_counter++;

  fprintf(stderr,"This message (hex): ");
  for(int i=0;i<offset;i++) fprintf(stderr,"%02x",msg_out[i]);
  fprintf(stderr,"\n");
  
  return offset;
}

// Bluetooth names can be 248 bytes long. In Android the string
// must be a valid UTF-8 string, so we will restrict ourselves to
// using only the lower 7 bits. It would be possible to use 7.something
// with a lot of effort, but it doesn't really seem justified.
#define BTNAME_MTU (248*7/8)

int main(int argc, char **argv)
{
  if (argc>3) {
    // set my_sid from argv[3]
    for(int i=0;i<32;i++) {
      char hex[3];
      hex[0]=argv[3][i*2];
      hex[1]=argv[3][i*2+1];
      hex[2]=0;
      my_sid[i]=strtoll(hex,NULL,16);
    }
  }

  while(1) {
    if (argc>2) load_rhizome_db(argv[1],argv[2]);

    unsigned char msg_out[BTNAME_MTU];
    
    update_my_message(BTNAME_MTU,msg_out);
    // The time it takes for a Bluetooth scan
    sleep(12);
  }
}
