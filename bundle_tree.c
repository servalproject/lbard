/*
  Serval Low-bandwidth asychronous Rhizome Demonstrator.
  Copyright (C) 2016 Serval Project Inc.

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

extern char *my_sid_hex;

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>

#include "sync.h"
#include "lbard.h"
#include "sha1.h"

int bundle_calculate_tree_key(sync_key_t *bundle_tree_key,
			      uint8_t sync_tree_salt[SYNC_SALT_LEN],
			      char *bid,
			      long long version,
			      long long length,
			      char *filehash)
{
  /*
    Calculate a sync key for this bundle.
    Sync keys are relatively short, only 64 bits, as this is still sufficient to
    maintain a very low probability of colissions, provided that each peer has less
    than 2^32 bundles.

    Ideally we would hash the entire manifest of a bundle, but that would require us
    to retrieve each manifest, and we would rather not require that.  So instead we
    will use the BID, length, version and filehash as inputs.  This combination means
    that in the unlikely event of a colission, updating the bundle is almost certain
    to resolve the colission.  Thus the natural human response of sending another
    message if the first doesn't get through is likely to resolve the problem.

    The relatively small key space does however leave us potentially vulnerable to a
    determined adversary to find coliding hashes to disrupt communications. We will
    need to do something about this in due course. This could probably be protected
    by using a random salt between pairs of peers when they do the sync process, so
    that colissions cannot be arranged ahead of time.

    Using a salt, and changing it periodically, would also provide implicit protection
    against colissions of any source, so it is probably a really good idea to
    implement.  The main disadvantage is that we need to calculate all the hashes for
    all the bundles we hold whenever we talk to a new peer.  We could, however,
    use a salt for all peers which we update periodically, to offer a good compromise
    between computational cost and protection against accidental and intentional
    colissions.
    
    Under this strategy, we need to periodically recalculate the sync key, i.e, hash,
    for each bundle we hold, and invalidate the sync tree for each peer when we do so.

    ... HOWEVER, we need both sides of a conversation to have the same salt, which
    wouldn't work under that scheme.

    So for now we will employ a salt, but it will probably be fixed until we decide on
    a good solution to this set of problems.
  */

  char lengthstring[80];
  snprintf(lengthstring,80,"%llx:%llx",length,version);
  
  struct sha1nfo sha1;  
  sha1_init(&sha1);
  sha1_write(&sha1,(const char *)sync_tree_salt,SYNC_SALT_LEN);
  sha1_write(&sha1,bid,strlen(bid));
  sha1_write(&sha1,filehash,strlen(filehash));
  sha1_write(&sha1,lengthstring,strlen(lengthstring));
  unsigned char *res=sha1_result(&sha1);
  bcopy(res,bundle_tree_key->key,KEY_LEN);
  return 0;  
}

int sync_tree_receive_message(struct peer_state *p,unsigned char *msg)
{
  int len=msg[1];

  if (debug_sync)
    printf("Receiving sync tree message of %d bytes\n",len);
      
  // Pull out the sync tree message for processing.
#define SYNC_MSG_HEADER_LEN 2
  int sync_bytes=len-SYNC_MSG_HEADER_LEN;

  if (debug_sync_keys) {
    char filename[1024];
    snprintf(filename,1024,"lbardkeys.%s.received_sync_message",my_sid_hex);
    FILE *f=fopen(filename,"a");
    fprintf(f,"%d:",len-2);
    for(int i=0;i<(len-2);i++) fprintf(f,"%02X ",msg[i+2]);
    fprintf(f,"\n");
    fclose(f);
  }

  
  sync_recv_message(sync_state,(void *)p,&msg[SYNC_MSG_HEADER_LEN], sync_bytes);
  
  return 0;
}

int sync_tree_send_message(int *offset,int mtu, unsigned char *msg_out)
{         
  uint8_t msg[256];
  int len=0;

  int bytes_available=mtu-SYNC_MSG_HEADER_LEN-(*offset);
  if (bytes_available<1) return -1;
  
  /* Send sync status message */
  msg[len++]='S'; // Sync message
  int length_byte_offset=len;
  msg[len++]=0; // place holder for length
  assert(len==SYNC_MSG_HEADER_LEN);

  int used=sync_build_message(sync_state,&msg[len],bytes_available);

  if (debug_sync_keys) {
    char filename[1024];
    snprintf(filename,1024,"lbardkeys.%s.sent_sync_message",my_sid_hex);
    FILE *f=fopen(filename,"a");
    fprintf(f,"%d:",used);
    for(int i=0;i<used;i++) fprintf(f,"%02X ",msg[len+i]);
    fprintf(f,"\n");
    fclose(f);
  }
  
  len+=used;
  // Record the length of the field
  msg[length_byte_offset]=len;
  append_bytes(offset,mtu,msg_out,msg,len);

  // Record in retransmit buffer
  // printf("Sending sync message (length now = $%02x, used %d)\n",*offset,used);

  
  return 0;
}

int sync_append_some_bundle_bytes(int bundle_number,int start_offset,int len,
				  unsigned char *p, int is_manifest,
				  int *offset,int mtu,unsigned char *msg,
				  int target_peer)
{
  int max_bytes=mtu-(*offset)-21;
  int bytes_available=len-start_offset;
  int actual_bytes=0;
  int end_of_item=0;

  // If we can't announce even one byte, we should just give up.
  if (start_offset>0xfffff) {
    max_bytes-=2; if (max_bytes<0) max_bytes=0;
  }
  if (max_bytes<1) return -1;

  // Work out number of bytes to include in announcement
  if (bytes_available<max_bytes) {
    actual_bytes=bytes_available;
    end_of_item=0;
  } else {
    actual_bytes=max_bytes;
    end_of_item=1;
  }
  // Make sure byte count fits in 11 bits.
  if (actual_bytes>0x7ff) actual_bytes=0x7ff;

  if (actual_bytes<0) return -1;
  
  // Generate 4 byte offset block (and option 2-byte extension for big bundles)
  long long offset_compound=0;
  offset_compound=(start_offset&0xfffff);
  offset_compound|=((actual_bytes&0x7ff)<<20);
  if (is_manifest) offset_compound|=0x80000000;
  offset_compound|=((start_offset>>20LL)&0xffffLL)<<32LL;

  // Now write the 23/25 byte header and actual bytes into output message
  // BID prefix (8 bytes)
  if (start_offset>0xfffff)
    msg[(*offset)++]='P'+end_of_item;
  else 
    msg[(*offset)++]='p'+end_of_item;

  // Intended recipient
  msg[(*offset)++]=peer_records[target_peer]->sid_prefix[0];
  msg[(*offset)++]=peer_records[target_peer]->sid_prefix[1];
  
  for(int i=0;i<8;i++) msg[(*offset)++]=bundles[bundle_number].bid_bin[i];
  // Bundle version (8 bytes)
  for(int i=0;i<8;i++)
    msg[(*offset)++]=(cached_version>>(i*8))&0xff;
  // offset_compound (4 bytes)
  for(int i=0;i<4;i++)
    msg[(*offset)++]=(offset_compound>>(i*8))&0xff;
  if (start_offset>0xfffff) {
    for(int i=4;i<6;i++)
      msg[(*offset)++]=(offset_compound>>(i*8))&0xff;
  }

  bcopy(p,&msg[(*offset)],actual_bytes);
  (*offset)+=actual_bytes;

  /* Advance the cursor for sending this bundle to all other peers if their cursor
     sits within the window we have just sent. */
  for(int pn=0;pn<peer_count;pn++) {
    if (pn!=target_peer&&peer_records[pn]) {
      if (peer_records[pn]->tx_bundle==bundle_number) {
	if (is_manifest) {
	  if ((peer_records[pn]->tx_bundle_manifest_offset>=start_offset)
	      &&(peer_records[pn]->tx_bundle_manifest_offset<(start_offset+actual_bytes)))
	    peer_records[pn]->tx_bundle_manifest_offset=(start_offset+actual_bytes);
	} else {
	  if ((peer_records[pn]->tx_bundle_body_offset>=start_offset)
	      &&(peer_records[pn]->tx_bundle_body_offset<(start_offset+actual_bytes))) {
	    fprintf(stderr,"T+%lldms : Cursor advance from %d to %d, due to sending [%d..%d].\n",
		    gettime_ms()-start_time,
		    peer_records[pn]->tx_bundle_body_offset,(start_offset+actual_bytes),
		    start_offset,(start_offset+actual_bytes)
		    );
	    peer_records[pn]->tx_bundle_body_offset=(start_offset+actual_bytes);
	  }
	}
      }
    }
  }
  
  if (debug_announce) {
    printf("T+%lldms : Announcing for %s* ",gettime_ms()-start_time,
	    peer_records[target_peer]->sid_prefix);
    for(int i=0;i<8;i++) fprintf(stderr,"%c",bundles[bundle_number].bid_hex[i]);
    printf("* (priority=0x%llx) version %lld %s segment [%d,%d)\n",
	    bundles[bundle_number].last_priority,
	    bundles[bundle_number].version,
	    is_manifest?"manifest":"payload",
	    start_offset,start_offset+actual_bytes);
  }

  char status_msg[1024];
  snprintf(status_msg,1024,"Announcing %c%c%c%c%c%c%c%c* version %lld %s segment [%d,%d)",
	   bundles[bundle_number].bid_hex[0],bundles[bundle_number].bid_hex[1],
	   bundles[bundle_number].bid_hex[2],bundles[bundle_number].bid_hex[3],
	   bundles[bundle_number].bid_hex[4],bundles[bundle_number].bid_hex[5],
	   bundles[bundle_number].bid_hex[6],bundles[bundle_number].bid_hex[7],
	   bundles[bundle_number].version,
	   is_manifest?"manifest":"payload",
	   start_offset,start_offset+actual_bytes);
  status_log(status_msg);

  return actual_bytes;
}


int sync_announce_bundle_piece(int peer,int *offset,int mtu,
			       unsigned char *msg,
			       char *sid_prefix_hex,
			       char *servald_server, char *credential)
{
  int bundle_number=peer_records[peer]->tx_bundle;
  if (bundle_number<0) return -1;
  
  if (prime_bundle_cache(bundle_number,
			 sid_prefix_hex,servald_server,credential)) {
    peer_records[peer]->tx_cache_errors++;
    if (peer_records[peer]->tx_cache_errors>MAX_CACHE_ERRORS)
      {
	sync_dequeue_bundle(peer_records[peer],peer_records[peer]->tx_bundle);
      }
    return -1;
  }
  else
    peer_records[peer]->tx_cache_errors=0;


  
  // Mark manifest all sent once we get to the end
  if (peer_records[peer]->tx_bundle_manifest_offset>=cached_manifest_encoded_len)
    peer_records[peer]->tx_bundle_manifest_offset=1024;

  // Send piece of manifest, if required
  if (peer_records[peer]->tx_bundle_manifest_offset<cached_manifest_encoded_len) {
    fprintf(stderr,"  manifest_offset=%d, manifest_len=%d\n",
	    peer_records[peer]->tx_bundle_manifest_offset,
	    cached_manifest_encoded_len);
    int start_offset=peer_records[peer]->tx_bundle_manifest_offset;
    int bytes =
      sync_append_some_bundle_bytes(bundle_number,start_offset,
				    cached_manifest_encoded_len,
				    &cached_manifest_encoded[start_offset],1,
				    offset,mtu,msg,peer);
    if (bytes>0)
      peer_records[peer]->tx_bundle_manifest_offset+=bytes;

  }

  // Announce the length of the body if we have finished sending the manifest,
  // but not yet started on the body.  This is really just to help monitoring
  // the progress of transfers for debugging.  The transfer process will automatically
  // detect the end of the bundle when the last piece is received.
  if (peer_records[peer]->tx_bundle_manifest_offset>=cached_manifest_encoded_len) {
    // Send length of body?
    if ((!peer_records[peer]->tx_bundle_body_offset)
	||(peer_records[peer]->tx_bundle_body_offset>=cached_body_len))
      {
	fprintf(stderr,"T+%lldms : Sending length of bundle %s\n",
		gettime_ms()-start_time,
		bundles[bundle_number].bid_hex);
	if ((mtu-*offset)>(1+8+8+4)) {
	  // Announce length of bundle
	  msg[(*offset)++]='L';
	  // Bundle prefix (8 bytes)
	  for(int i=0;i<8;i++) msg[(*offset)++]=bundles[bundle_number].bid_bin[i];
	  // Bundle version (8 bytes)
	  for(int i=0;i<8;i++)
	    msg[(*offset)++]=(cached_version>>(i*8))&0xff;
	  // Length (4 bytes)
	  msg[(*offset)++]=(bundles[bundle_number].length>>0)&0xff;
	  msg[(*offset)++]=(bundles[bundle_number].length>>8)&0xff;
	  msg[(*offset)++]=(bundles[bundle_number].length>>16)&0xff;
	  msg[(*offset)++]=(bundles[bundle_number].length>>24)&0xff;
	}
      }
    {
      // Send some of the body
      int start_offset=peer_records[peer]->tx_bundle_body_offset;
      int bytes =
	sync_append_some_bundle_bytes(bundle_number,start_offset,cached_body_len,
				      &cached_body[start_offset],0,
				      offset,mtu,msg,peer);

      if (bytes>0)
	peer_records[peer]->tx_bundle_body_offset+=bytes;      
    }
  }

  
  // If we have sent to the end of the bundle, then start again from the beginning,
  // until the peer acknowledges that they have received it all (or tells us to
  // start sending again from a different part of the bundle).
  if ((peer_records[peer]->tx_bundle_body_offset>=bundles[bundle_number].length)
      &&(peer_records[peer]->tx_bundle_manifest_offset>=cached_manifest_encoded_len))
    {
      peer_records[peer]->tx_bundle_body_offset=0;
      peer_records[peer]->tx_bundle_manifest_offset=0;
      fprintf(stderr,"T+%lldms : Resending bundle %s from the start.\n",
	      gettime_ms()-start_time,
	      bundles[bundle_number].bid_hex);

    }
  
  return 0;
}

int sync_tree_send_data(int *offset,int mtu, unsigned char *msg_out,int peer,
			char *sid_prefix_bin,char *servald_server,char *credential)
{
  /*
    Send a piece of the bundle (manifest or body) to this peer, for the highest
    priority bundle that we have that we believe that they don't have.
    (If they have it, then they will acknowledge the entirety of it, allowing us
    to advance to the next bundle.)

  */
  if (peer_records[peer]->tx_bundle>-1)
    {
      // Try to also send a piece of body, even if we have already stuffed some
      // manifest in, because we might still have space.
      
      sync_announce_bundle_piece(peer,offset,mtu,msg_out,
				 sid_prefix_bin,servald_server,credential);
    }
  return 0;
}

#define REPORT_QUEUE_LEN 32
#define MAX_REPORT_LEN 64
int report_queue_length=0;
uint8_t report_queue[REPORT_QUEUE_LEN][MAX_REPORT_LEN];
uint8_t report_lengths[REPORT_QUEUE_LEN];
struct peer_state *report_queue_peers[REPORT_QUEUE_LEN];
int report_queue_partials[REPORT_QUEUE_LEN];
char *report_queue_message[REPORT_QUEUE_LEN];

int sync_by_tree_stuff_packet(int *offset,int mtu, unsigned char *msg_out,
			      char *sid_prefix_bin,
			      char *servald_server,char *credential)
{
  // Stuff packet as full as we can with data for as many peers as we can.
  // In practice, we will likely fill it on the first peer, but let's not
  // waste a packet if we have something we can stuff in.

  // First of all, tell any peers any acknowledgement messages that are required.
  while (report_queue_length&&((*offset)<(mtu-MAX_REPORT_LEN))) {
    report_queue_length--;
    if (append_bytes(offset,mtu,msg_out,report_queue[report_queue_length],
		     report_lengths[report_queue_length])) {
      fprintf(stderr,"Tried to send report_queue message '%s' to %s*, but append_bytes reported no more space.\n",
	      report_queue_message[report_queue_length],
	      report_queue_peers[report_queue_length]->sid_prefix);
      report_queue_length++;
    } else {
    fprintf(stderr,"T+%lldms : Flushing report from queue, %d remaining.\n",
	    gettime_ms()-start_time,	    
	    report_queue_length);
    fprintf(stderr,"Sent report_queue message '%s' to %s*\n",
	    report_queue_message[report_queue_length],
	    report_queue_peers[report_queue_length]->sid_prefix);
    free(report_queue_message[report_queue_length]);
    report_queue_message[report_queue_length]=NULL;
    }
  }
  
  int count=10; if (count>peer_count) count=peer_count;

  /* Try sending something new.
     Sync trees, and if space remains (because we have synchronised trees),
     then try sending a piece of a bundle, if any */
  int sync_not_sent=1;
  if (random()&1) {
    sync_not_sent=0;
    sync_tree_send_message(offset,mtu,msg_out);
  }
  
  while((*offset)<(mtu-16)) {
    if ((count--)<0) break;
    int peer=random_active_peer();
    if (peer<0) break;
    int space=mtu-(*offset);
    if (space>10) {
      sync_tree_send_data(offset,mtu,msg_out,peer,
			  sid_prefix_bin,servald_server,credential);
    } else {
      // No space -- can't do anything
    }
  }  

  if (sync_not_sent)
    // Don't waste any space: sync what we can
    sync_tree_send_message(offset,mtu,msg_out);
  
  return 0;
}

int sync_tree_populate_with_our_bundles()
{
  for(int i=0;i<bundle_count;i++)
    sync_add_key(sync_state,&bundles[i].sync_key,&bundles[i]);
  return 0;
}


int sync_build_bar_in_slot(int slot,unsigned char *bid_bin,
			   long long bundle_version)
{
  int ofs=0;
  report_queue[slot][ofs++]='B';

  // BID prefix
  for(int i=0;i<8;i++) report_queue[slot][ofs++]=bid_bin[i];
  // Bundle Version
  for(int i=0;i<8;i++) report_queue[slot][ofs++]=(bundle_version>>(i*8))&0xff;
  // Dummy recipient + size byte
  for(int i=0;i<5;i++) report_queue[slot][ofs++]=(bundle_version>>(i*8))&0xff;

  report_lengths[slot]=ofs;
  assert(ofs<MAX_REPORT_LEN);
  return 0;
}

int sync_tell_peer_we_have_this_bundle(int peer, int bundle)
{
  return sync_tell_peer_we_have_bundle_by_id(peer,bundles[bundle].bid_bin,
					     bundles[bundle].version);
}
  
int sync_tell_peer_we_have_bundle_by_id(int peer,unsigned char *bid,long long version)
{
  int slot=report_queue_length;

  for(int i=0;i<report_queue_length;i++) {
    if (report_queue_peers[i]==peer_records[peer]) {
      // We already want to tell this peer something.
      // We should only need to tell a peer one thing at a time.
      slot=i; break;
    }
  }
  
  if (slot>=REPORT_QUEUE_LEN) slot=random()%REPORT_QUEUE_LEN;

  // Mark utilisation of slot, so that we can flush out stale messages
  report_queue_partials[slot]=-1;
  report_queue_peers[slot]=peer_records[peer];

  sync_build_bar_in_slot(slot,bid,version);

  if (report_queue_message[slot]) {
    fprintf(stderr,"Replacing report_queue message '%s' with 'BAR'\n",
	    report_queue_message[slot]);
    free(report_queue_message[slot]);
    report_queue_message[slot]=NULL;
  } else
    fprintf(stderr,"Setting report_queue message to 'BAR'\n");
  report_queue_message[slot]=strdup("BAR");
  
  if (slot>=report_queue_length) report_queue_length=slot+1;

  return 0;
}

unsigned char bin_prefix[8];
unsigned char *bid_prefix_hex_to_bin(char *hex)
{
  for(int i=0;i<8;i++) {
    char h[3]={hex[i*2+0],hex[i*2+1],0};
    bin_prefix[i]=strtoll(h,NULL,16);
  }
  return bin_prefix;
}

int sync_tell_peer_we_have_the_bundle_of_this_partial(int peer, int partial)
{

  return sync_tell_peer_we_have_bundle_by_id
    (peer,bid_prefix_hex_to_bin(peer_records[peer]->partials[partial].bid_prefix),
     peer_records[peer]->partials[partial].bundle_version);  
}


/* Find the first byte missing in the following segment list.
   Basically this boils down to being either byte 0, or the
   first byte after the first segment. 

   However, we actually want to randomise the byte we ask for,
   so that if a peer is sending to multiple peers, that we can
   encourage them to send unique content.  This ideally requires
   that we know who a piece is addressed to. But in the very
   least, we should pick a random starting point that is adjacent
   to one of our partial pieces.  However, we need to take care to
   not make the sender think that we have it all.
*/
int partial_first_missing_byte(struct segment_list *s)
{
  int add_zero=1;
  
  int candidates[16];
  int candidate_count=0;
  
  // Walk the segment list. Adjacent segments should be merged,
  // so the offset following each segment is a valid candidate,
  // except if a candidate is the end of the file.
  while(s) {
    if (!s->start_offset) add_zero=0;
    if (candidate_count<16)
      candidates[candidate_count++]=s->start_offset+s->length;
    s=s->next;
  }
  if ((candidate_count<16)&&(add_zero)) candidates[candidate_count++]=0;
  
  // The values should be in descending order. Don't ask for highest value,
  // incase it signals the end of the bundle (we don't necessarily know the
  // payload length during reception).  Thus only ask for the end point if
  // there are no other alternatives.
  if (candidate_count>1)
    return candidates[1+random()%(candidate_count-1)];
  else return candidates[0];
}

int sync_schedule_progress_report(int peer, int partial)
{
  int slot=report_queue_length;

  for(int i=0;i<report_queue_length;i++) {
    if (report_queue_peers[i]==peer_records[peer]) {
      // We already want to tell this peer something.
      // We should only need to tell a peer one thing at a time.
      slot=i; break;
    }
  }
  
  if (slot>=REPORT_QUEUE_LEN) slot=random()%REPORT_QUEUE_LEN;

  // Mark utilisation of slot, so that we can flush out stale messages
  report_queue_partials[slot]=partial;
  report_queue_peers[slot]=peer_records[peer];

  int ofs=0;
  report_queue[slot][ofs++]='A';

  if (report_queue_message[slot]) {
    fprintf(stderr,"Replacing report_queue message '%s' with 'progress report'\n",
	    report_queue_message[slot]);
    free(report_queue_message[slot]);
    report_queue_message[slot]=NULL;
  } else {
    fprintf(stderr,"Setting report_queue message to 'progress report'\n");
  }
  report_queue_message[slot]=strdup("progress report");

  // BID prefix
  for(int i=0;i<8;i++) {
    int hex_value=0;
    char hex_string[3]={peer_records[peer]->partials[partial].bid_prefix[i*2+0],
			peer_records[peer]->partials[partial].bid_prefix[i*2+1],
			0};
    hex_value=strtoll(hex_string,NULL,16);
    report_queue[slot][ofs++]=hex_value;
  }
  
  // manifest and body offset
  int first_required_manifest_offset
    =partial_first_missing_byte(peer_records[peer]
				->partials[partial].manifest_segments);
  int first_required_body_offset
    =partial_first_missing_byte(peer_records[peer]
				->partials[partial].body_segments);
  report_queue[slot][ofs++]=first_required_manifest_offset&0xff;
  report_queue[slot][ofs++]=(first_required_manifest_offset>>8)&0xff;
  report_queue[slot][ofs++]=first_required_body_offset&0xff;
  report_queue[slot][ofs++]=(first_required_body_offset>>8)&0xff;
  report_queue[slot][ofs++]=(first_required_body_offset>>16)&0xff;
  report_queue[slot][ofs++]=(first_required_body_offset>>24)&0xff;

  report_lengths[slot]=ofs;
  assert(ofs<MAX_REPORT_LEN);
  if (slot>=report_queue_length) report_queue_length=slot+1;

  fprintf(stderr,
	  "T+%lldms : ACKing progress on transfer of %s* from %s. m_first=%d, b_first=%d\n",
	  gettime_ms()-start_time,
	  peer_records[peer]->partials[partial].bid_prefix,
	  peer_records[peer]->sid_prefix,
	  first_required_manifest_offset,
	  first_required_body_offset);    
  
  return 0;
}

int lookup_bundle_by_prefix_hex(char *prefix)
{
  int bundle;
  int i;
  for(bundle=0;bundle<bundle_count;bundle++) {
    for(i=0;i<8;i++) {
      if (prefix[i]!=bundles[bundle].bid_hex[i]) break;
    }
    if (i==8) return bundle;
  }
  return -1;
}

int lookup_bundle_by_prefix_bin_and_version(unsigned char *prefix, long long version)
{
  int bundle;
  int i;
  for(bundle=0;bundle<bundle_count;bundle++) {
    for(i=0;i<8;i++) {
      if (prefix[i]!=bundles[bundle].bid_bin[i]) break;
    }
    if (i==8) {
      if (bundles[bundle].version>=version)
	return bundle;
    }
  }
  return -1;
}


int sync_queue_bundle(struct peer_state *p,int bundle)
{
  struct bundle_record *b=&bundles[bundle];

  int priority=calculate_bundle_intrinsic_priority(b->bid_hex,
						   b->length,
						   b->version,
						   b->service,
						   b->recipient,
						   0);

  // TX queue has something in it.
  if (p->tx_bundle>=0) {
    if (priority>p->tx_bundle_priority) {
      // Bump current tx_bundle to TX queue, and substitute with this one.
      // (substitution happens below)
      peer_queue_bundle_tx(p,&bundles[p->tx_bundle],
			   p->tx_bundle_priority);
      p->tx_bundle=-1;
    } else {
      // Bump new bundle to TX queue
      peer_queue_bundle_tx(p,b,priority);
    }
  }

  // If nothing in the TX queue, just add it.
  // (also used to putting new bundle in the current TX slot if there was something
  // lower priority in there previously.)
  if (p->tx_bundle==-1) {
    p->tx_bundle=bundle;
    // Start body transmission at a random point, so that if we are sending the
    // bundle to multiple peers, we at least have a chance of not sending the same
    // piece to each in a redundant manner. It would be even better to have some
    // awareness of when we are doing this, so that we can optimise it. XXX
    // XXX - The benefit of this disappears as soon as the peer requests a
    // re-transmission, since it will start requesting from the earliest byte that it
    // lacks.
    p->tx_bundle_body_offset=random()%bundles[bundle].length;
    p->tx_bundle_manifest_offset=0;
    p->tx_bundle_priority=priority;
  }

  // peer_queue_list_dump(p);
  return 0;
}


int sync_dequeue_bundle(struct peer_state *p,int bundle)
{
  if (bundle==p->tx_bundle) {
    // Delete this entry in queue
    p->tx_bundle=-1;
    // Advance next in queue, if there is anything
    if (p->tx_queue_len) {
      printf("DEQUEUING:\n     %d more bundles in the queue. Next is bundle #%d\n",
	     p->tx_queue_len,p->tx_queue_bundles[0]);
      p->tx_bundle=p->tx_queue_bundles[0];
      p->tx_bundle_priority=p->tx_queue_priorities[0];
      p->tx_bundle_manifest_offset=0;
      p->tx_bundle_body_offset=0;      
      bcopy(&p->tx_queue_bundles[1],
	    &p->tx_queue_bundles[0],
	    sizeof(int)*p->tx_queue_len-1);
      bcopy(&p->tx_queue_priorities[1],
	    &p->tx_queue_priorities[0],
	    sizeof(int)*p->tx_queue_len-1);
      p->tx_queue_len--;
    } else {
      if (p->tx_queue_overflow) {
	/* TX queue overflowed at some point, and now we have
	   emptied the queue. This means that we need to restart
	   the tree synchronisation with this peer.  
	   XXX - In time, Jeremy will implement sync tree enumeration,
	   which will be a more efficient solution to this problem.
	   In the meantime, we will just change our instance ID, and also
	   the instance ID we have recorded for this peer, so that we
	   force a re-sync.
	*/
	p->instance_id=0xffffffff;
	my_instance_id=0;
	while(my_instance_id==0)
	  urandombytes((unsigned char *)&my_instance_id,sizeof(unsigned int));
      }
    }
  } else {
    // Wasn't the bundle on the list right now, so delete from in list.
    for(int i=0;i<p->tx_queue_len;i++) {
      if (bundle==p->tx_queue_bundles[i]) {
	// printf("Before deletion from in queue:\n");
	// peer_queue_list_dump(p);
	// Delete this entry in queue
	bcopy(&p->tx_queue_bundles[i+1],
	      &p->tx_queue_bundles[i],
	      sizeof(int)*p->tx_queue_len-i-1);
	bcopy(&p->tx_queue_priorities[i+1],
	      &p->tx_queue_priorities[i],
	      sizeof(int)*p->tx_queue_len-i-1);
	p->tx_queue_len--;
	// printf("After deletion from in queue:\n");
	// peer_queue_list_dump(p);
	return 0;
      }
    }
    
  }

  return 0;
}


int sync_parse_ack(struct peer_state *p,unsigned char *msg)
{
  // Get fields
  int manifest_offset=msg[9]|(msg[10]<<8);
  int body_offset=msg[11]|(msg[12]<<8)|(msg[13]<<16)|(msg[14]<<24);

  char bid_prefix_hex[8*2+1];
  snprintf(bid_prefix_hex,17,"%02X%02X%02X%02X%02X%02X%02X%02X",
	   msg[1],msg[2],msg[3],msg[4],msg[5],msg[6],msg[7],msg[8]);
  
  int bundle=lookup_bundle_by_prefix_hex(bid_prefix_hex);

  fprintf(stderr,"T+%lldms : SYNC ACK: %s* is asking for us to send from m=%d, p=%d of"
	  " %02x%02x%02x%02x%02x%02x%02x%02x (bundle #%d)\n",
	  gettime_ms()-start_time,
	  p?p->sid_prefix:"<null>",manifest_offset,body_offset,
	  msg[1],msg[2],msg[3],msg[4],msg[5],msg[6],msg[7],msg[8],
	  bundle);

  // Sanity check inputs, so that we don't mishandle memory.
  if (manifest_offset<0) manifest_offset=0;
  if (body_offset<0) body_offset=0;  

  if (bundle<0) return -1;  
  if (bundle==p->tx_bundle) {
    fprintf(stderr,"SYNC ACK: %s* is asking for us to send from m=%d, p=%d\n",
	    p->sid_prefix,manifest_offset,body_offset);
    p->tx_bundle_manifest_offset=manifest_offset;
    p->tx_bundle_body_offset=body_offset;      
  } else {
    fprintf(stderr,"SYNC ACK: Ignoring, because we are sending bundle #%d, and request is for bundle #%d\n",p->tx_bundle,bundle);
    fprintf(stderr,"          Requested BID/version = %s/%lld\n",
	    bundles[bundle].bid_hex, bundles[bundle].version);
    fprintf(stderr,"                 TX BID/version = %s/%lld\n",
	    bundles[p->tx_bundle].bid_hex, bundles[p->tx_bundle].version);
  }

  return 0;
}

void peer_has_this_key(void *context, void *peer_context, const sync_key_t *key)
{
  struct peer_state *p=(struct peer_state *)peer_context;

  // Peer has something that we want.
  if (0) printf(">>> Peer %s* is HAS some bundle that we don't have.\n",
		p->sid_prefix);

}

void peer_now_has_this_key(void *context, void *peer_context,void *key_context,
			   const sync_key_t *key)
{
  // Peer has something, that we also have. 
  // We should stop sending it to them, if we were trying.

  struct peer_state *p=(struct peer_state *)peer_context;
  struct bundle_record *b=(struct bundle_record*)key_context;

  if (0)
    printf(">>> Peer %s* is now has bundle %s*\n"
	   "    service=%s, version=%lld\n"
	   "    sender=%s,\n"
	   "    recipient=%s\n",
	   p->sid_prefix,
	   b->bid_hex,b->service,b->version,b->sender,b->recipient);

  sync_dequeue_bundle(p,b->index);

}


void peer_does_not_have_this_key(void *context, void *peer_context,void *key_context,
				 const sync_key_t *key)
{
  // We need to send something to a peer
  
  struct peer_state *p=(struct peer_state *)peer_context;
  struct bundle_record *b=(struct bundle_record*)key_context;

  if (0)
    printf(">>> Peer %s* is missing bundle %s*\n"
	   "    service=%s, version=%lld\n"
	   "    sender=%s,\n"
	   "    recipient=%s\n",
	   p->sid_prefix,
	   b->bid_hex,b->service,b->version,b->sender,b->recipient);

  if (debug_sync_keys) {
    char filename[1024];
    snprintf(filename,1024,"lbardkeys.%s.needs.to.send.to.%s",my_sid_hex,p->sid_prefix);
    FILE *f=fopen(filename,"a");
    fprintf(f,"%02X%02X%02X%02X%02X%02X%02X%02X:%s:%016llX\n",
	    key->key[0],key->key[1],key->key[2],key->key[3],
	    key->key[4],key->key[5],key->key[6],key->key[7],
	    b->bid_hex,b->version);
    fclose(f);
  }
    
  sync_queue_bundle(p,b->index);
  
  return;  
}


int sync_setup()
{
  sync_state = sync_alloc_state(NULL,
				peer_has_this_key,
				peer_does_not_have_this_key,
				peer_now_has_this_key);
  return 0;
}

#define MAX_RECENT_BUNDLES 128
#define RECENT_BUNDLE_TIMEOUT (4*60)
struct recent_bundle recent_bundles[MAX_RECENT_BUNDLES];
int recent_bundle_count=0;

int sync_remember_recently_received_bundle(char *bid_prefix, long long version)
{
  int first_timed_out=-1;
  int i;
  for(i=0;i<recent_bundle_count;i++)
    if (!strcasecmp(bid_prefix,recent_bundles[i].bid_prefix)) {
      if (version>=recent_bundles[i].bundle_version)
	recent_bundles[i].bundle_version=version;
      recent_bundles[i].timeout=time(0)+RECENT_BUNDLE_TIMEOUT;
      return 0;
    } else {
      if (recent_bundles[i].timeout<time(0)) first_timed_out=i;
    }
  if (recent_bundle_count>=MAX_RECENT_BUNDLES) {
    if (first_timed_out==-1) i=random()%MAX_RECENT_BUNDLES;
    else i=first_timed_out;
    free(recent_bundles[i].bid_prefix); recent_bundles[i].bid_prefix=NULL;
  } else {
    i=recent_bundle_count;
    recent_bundle_count++;
  }

  recent_bundles[i].bid_prefix=strdup(bid_prefix);
  recent_bundles[i].bundle_version=version;
  recent_bundles[i].timeout=time(0)+RECENT_BUNDLE_TIMEOUT;

  fprintf(stderr,"recent_bundle_count now %d\n",recent_bundle_count);
  return 0;
}

int sync_is_bundle_recently_received(char *bid_prefix, long long version)
{
  for(int i=0;i<recent_bundle_count;i++) {
    
    if (!strcasecmp(bid_prefix,recent_bundles[i].bid_prefix)) {
      if (version<=recent_bundles[i].bundle_version)
	if (recent_bundles[i].timeout>=time(0)) {
	  printf("Ignoring %s*/%lld because we recently received %s*/%lld\n",
		 bid_prefix,version,
		 recent_bundles[i].bid_prefix,
		 recent_bundles[i].bundle_version);
	  return 1;
	} else
	  return 0;
      else return 0;
    }
  }
  return 0;  
}
