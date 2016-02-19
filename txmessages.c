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
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "sync.h"
#include "lbard.h"

int log2ish(int value)
{
  int result=0;
  while(value) {
    result++; value=value>>1;
  }
  return result;
}

long long size_byte_to_length(unsigned char size_byte)
{
  return 1<<size_byte;
}


int append_bytes(int *offset,int mtu,unsigned char *msg_out,
		 unsigned char *data,int count)
{
  if (((*offset)+count)<=mtu) {
    bcopy(data,&msg_out[*offset],count);
    return 0;
  }
  return -1;
}

#ifdef SYNC_BY_BAR
int append_bar(int bundle_number,int *offset,int mtu,unsigned char *msg_out)
{
  // BAR consists of:
  // 8 bytes : BID prefix
  // 8 bytes : version
  // 4 bytes : recipient prefix
  // 1 byte : log2(ish) size and meshms flag
  
  for(int i=0;i<8;i++)
    msg_out[(*offset)++]=hex_byte_value(&bundles[bundle_number].bid[i*2]);
  for(int i=0;i<8;i++)
    msg_out[(*offset)++]=(bundles[bundle_number].version>>(i*8))&0xff;
  for(int i=0;i<4;i++)
    msg_out[(*offset)++]=hex_byte_value(&bundles[bundle_number].recipient[i*2]);
  int size_byte=log2ish(bundles[bundle_number].length);
  if ((!strcasecmp("MeshMS1",bundles[bundle_number].service))
      ||(!strcasecmp("MeshMS2",bundles[bundle_number].service)))
    size_byte&=0x7f;
  else
    size_byte|=0x80;
  msg_out[(*offset)++]=size_byte;

  char status_msg[1024];
  snprintf(status_msg,1024,"Announcing BAR %c%c%c%c%c%c%c%c* version %lld [%s]",
	   bundles[bundle_number].bid[0],bundles[bundle_number].bid[1],
	   bundles[bundle_number].bid[2],bundles[bundle_number].bid[3],
	   bundles[bundle_number].bid[4],bundles[bundle_number].bid[5],
	   bundles[bundle_number].bid[6],bundles[bundle_number].bid[7],
	   bundles[bundle_number].version,	   
	   bundles[bundle_number].service);
  status_log(status_msg);

  
  return 0;
}
#endif

int announce_bundle_piece(int bundle_number,int *offset,int mtu,unsigned char *msg,
			  char *prefix,char *servald_server, char *credential,
			  int target_peer)
{
  if (prime_bundle_cache(bundle_number,
			 prefix,servald_server,credential)) return -1;

  /*
    We need to prefix any piece with the BID prefix, BID version, the offset
    of the content, which will also include a flag to indicate if it is content
    from the manifest or body.  This entails the following:
    Piece header - 1 byte
    intended recipient prefix - 2 bytes
    BID prefix - 8 bytes
    bundle version - 8 bytes
    offset & manifest/body flag & length - 4 bytes
    (20 bits length, 1 bit manifest/body flag, 11 bits length)

    Total = 23 bytes.

    If start_offset>0xfffff, then 2 extra bytes are used for the upper bytes of the
    starting offset.    

    When we are about to begin sending the body, we send a message that indicates the
    length of the current bundle so that the far end knows, and can factor it into its
    prioritisation of transfers when requesting retransmissions. We don't need to do
    this for journal bundles, however, because their length is the same as their 
    version
  */

#ifdef SYNC_BY_BAR
  if ((cached_version>0x100000000LL)
      &&(bundles[bundle_number].last_manifest_offset_announced
	 >=cached_manifest_encoded_len)
      &&(!bundles[bundle_number].last_offset_announced))
    {
      printf("Announcing length of bundle #%d (tx offsets=%d,%d of %d,%d)\n",
	     bundle_number,
	     bundles[bundle_number].last_manifest_offset_announced,
	     bundles[bundle_number].last_offset_announced,
	     cached_manifest_encoded_len,
	     cached_body_len);
      if ((mtu-*offset)>(1+8+8+4)) {
	// Announce length of bundle
	msg[(*offset)++]='L';
	// Bundle prefix (8 bytes)
	for(int i=0;i<8;i++)
	  msg[(*offset)++]=hex_byte_value(&bundles[bundle_number].bid[i*2]);
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
#endif

  // Check that we still have space after
  int max_bytes=mtu-(*offset)-21;
  if (max_bytes<1) return 0;
  
  int is_manifest=0;
  unsigned char *p=NULL;
  int actual_bytes=0;
  int bytes_available=0;
  int start_offset=0;
  
  // For journaled bundles, update the first body announced offset to begin at the
  // first byte that the recipient (if they are a peer) has received.
  // If the recipient is not a peer, then we send from the first byte that any of
  // our peers has yet to receive.
  // By checking this each time we send a piece, we will automatically skip bytes
  // that we have just heard about a peer having received.

  /* Is the bundle a journalled bundle?
     This only works if we sync by BARs, as in sync by key mode we don't have
     direct information about the versions of a given bundle that a peer might
     have. That said, the peer will automatically acknowledge the bytes it has,
     so the end result is only one wasted packet per bundle.  Not ideal, but 
     not unreasonable either. 
  */
  if (cached_version<0x100000000LL) {
    long long first_byte=0;
#ifdef SYNC_BY_BAR
    int j;
    for(j=0;j<peer_count;j++) {
      if (!strncmp(bundles[bundle_number].recipient,
		   peer_records[j]->sid_prefix,(8*2))) {
	// Bundle is address to a peer.
	int k;
	for(k=0;k<peer_records[j]->bundle_count;k++) {
	  if (!strncmp(peer_records[j]->bid_prefixes[k],bundles[bundle_number].bid,
		       8*2)) {
	    // Peer knows about this bundle, but which version?
	    first_byte=peer_records[j]->versions[k];
	  }
	}
      }
    }
    if (!first_byte) {
      // Recipient has no bytes or is not a peer, so now do the overall scan to see
      // if all our peers have at least some bytes, and if so skip them.
      first_byte=cached_body_len;
      for(j=0;j<peer_count;j++) {
	int k;
	for(k=0;k<peer_records[j]->bundle_count;k++) {
	  if (!strncmp(peer_records[j]->bid_prefixes[k],bundles[bundle_number].bid,
		       8*2)) {
	    // Peer knows about this bundle, but which version?
	    if (peer_records[j]->versions[k]<first_byte)
	      first_byte=peer_records[j]->versions[k];
	    break;
	  }
	}
	if (k==peer_records[j]->bundle_count) {
	  // Peer does not have this bundle, so we must start from the beginning.
	  first_byte=0;
	}	
      }
    }
#endif    

    // If no peers, we can't make inferences about who has what bytes
    if (!peer_count) first_byte=0;
    
    if (bundles[bundle_number].last_offset_announced<first_byte) {
      fprintf(stderr,"Skipping from byte %lld straight to %lld, because recipient or all peers have the intervening bytes\n",
	      bundles[bundle_number].last_offset_announced,first_byte);
      bundles[bundle_number].last_offset_announced=first_byte;
    }
  }

  int end_of_item=0;
  
  if (bundles[bundle_number].last_manifest_offset_announced<cached_manifest_encoded_len) {
    // Send some manifest
    bytes_available=cached_manifest_encoded_len-
      bundles[bundle_number].last_manifest_offset_announced;
    is_manifest=1;
    start_offset=bundles[bundle_number].last_manifest_offset_announced;
    p=&cached_manifest_encoded[bundles[bundle_number].last_manifest_offset_announced];
  } else if (bundles[bundle_number].last_offset_announced<cached_body_len) {
    // Send some body
    bytes_available=cached_body_len-
      bundles[bundle_number].last_offset_announced;
    p=&cached_body[bundles[bundle_number].last_offset_announced];    
    start_offset=bundles[bundle_number].last_offset_announced;
  }

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

  // Generate 4 byte offset block (and option 2-byte extension for big bundles)
  long long offset_compound=0;
  offset_compound=(start_offset&0xfffff);
  offset_compound|=((actual_bytes&0x7ff)<<20);
  if (is_manifest) offset_compound|=0x80000000;
  offset_compound|=((start_offset>>20LL)&0xffffLL)<<32LL;

  // Now write the 21/23 byte header and actual bytes into output message
  // BID prefix (8 bytes)
  if (start_offset>0xfffff)
    msg[(*offset)++]='P'+end_of_item;
  else 
    msg[(*offset)++]='p'+end_of_item;

  // Intended recipient
  msg[(*offset)++]=peer_records[target_peer]->sid_prefix[0];
  msg[(*offset)++]=peer_records[target_peer]->sid_prefix[1];  
  
  for(int i=0;i<8;i++)
    msg[(*offset)++]=hex_byte_value(&bundles[bundle_number].bid[i*2]);
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

  if (debug_announce) {
    fprintf(stderr,"Announcing ");
    for(int i=0;i<8;i++) fprintf(stderr,"%c",bundles[bundle_number].bid[i]);
    fprintf(stderr,"* (priority=0x%llx) version %lld %s segment [%d,%d)\n",
	    bundles[bundle_number].last_priority,
	    bundles[bundle_number].version,
	    is_manifest?"manifest":"payload",
	    start_offset,start_offset+actual_bytes);
  }

  char status_msg[1024];
  snprintf(status_msg,1024,"Announcing %c%c%c%c%c%c%c%c* version %lld %s segment [%d,%d)",
	   bundles[bundle_number].bid[0],bundles[bundle_number].bid[1],
	   bundles[bundle_number].bid[2],bundles[bundle_number].bid[3],
	   bundles[bundle_number].bid[4],bundles[bundle_number].bid[5],
	   bundles[bundle_number].bid[6],bundles[bundle_number].bid[7],
	   bundles[bundle_number].version,
	   is_manifest?"manifest":"payload",
	   start_offset,start_offset+actual_bytes);
  status_log(status_msg);
	     
  // Update offset announced
  if (is_manifest) {
    bundles[bundle_number].last_manifest_offset_announced+=actual_bytes;
  } else {
    bundles[bundle_number].last_offset_announced+=actual_bytes;

    if (bundles[bundle_number].last_offset_announced==cached_body_len) {
      // If we have reached the end, then mark this bundle as having been announced.
      if (debug_pull) fprintf(stderr,"*** Marking %c%c%c%c%c%c%c%c* as having been sent.\n",
			      bundles[bundle_number].bid[0],
			      bundles[bundle_number].bid[1],
			      bundles[bundle_number].bid[2],
			      bundles[bundle_number].bid[3],
			      bundles[bundle_number].bid[4],
			      bundles[bundle_number].bid[5],
			      bundles[bundle_number].bid[6],
			      bundles[bundle_number].bid[7]
			      );
      bundles[bundle_number].last_announced_time=time(0);
      // XXX - Race condition exists where bundle version could be updated while we
      // are announcing it.  By caching the version number, we reduce, but do not
      // eliminate this risk, but at least the recipient will realise if they are being
      // given a mix of pieces.
      bundles[bundle_number].last_version_of_manifest_announced=
	cached_version;

      // Then reset offsets for announcing next time
      bundles[bundle_number].last_offset_announced=0;
      bundles[bundle_number].last_manifest_offset_announced=0;
    }
  }
  
  return 0;

}

/* Our time stratum. 0xff00 means that we have not had any external time input,
   other values are hops from a time authority (who may or may not actually be
   accurate).  The time is used for logging purposes, and while more accurate would
   be better, having the time to within a few seconds is a huge step up from every
   Mesh Extender thinking it is January 1970.
*/
int my_time_stratum=0xff00;

int message_counter=0;
int update_my_message(int serialfd,
		      unsigned char *my_sid, int mtu,unsigned char *msg_out,
		      char *servald_server,char *credential)
{
#ifdef SYNC_BY_BAR
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
#else
  /*
    With sync tree method, we don't send anything other than a "here we are and this
    is my root tree hash" by default. Only when we register a peer do we start trying
    to talk further. We limit transmissions to syncing trees, and acknowledging frames,
    unless there are bundles which we have identified need to be sent to them.
  */
#endif

  // Build output message

  if (mtu<64) return -1;
  
  // Clear message
  bzero(msg_out,mtu);

  // Put prefix of our SID in first 6 bytes.
  char prefix[7];
  for(int i=0;i<6;i++) { msg_out[i]=my_sid[i]; prefix[i]=my_sid[i]; }
  prefix[6]=0;

  // Put 2-byte message counter.
  // lower 15 bits is message counter.
  // the 16th bit indicates if this message is a retransmission
  // (always clear when constructing the message).
  msg_out[6]=message_counter&0xff;
  msg_out[7]=(message_counter>>8)&0x7f;

  int offset=8;

  if (random()%10) {
    // Occassionally announce our time
    // T + (our stratum) + (64 bit seconds since 1970) +
    // + (24 bit microseconds)
    // = 1+1+8+3 = 13 bytes
    struct timeval tv;
    gettimeofday(&tv,NULL);    
    
    msg_out[offset++]='T';
    msg_out[offset++]=my_time_stratum>>8;
    for(int i=0;i<8;i++)
      msg_out[offset++]=(tv.tv_sec>>(i*8))&0xff;
    for(int i=0;i<3;i++)
      msg_out[offset++]=(tv.tv_usec>>(i*8))&0xff;    
  }

#ifdef SYNC_BY_BAR
  // Put one or more BARs
  int bar_number=find_highest_priority_bar();
  if (bundle_count&&((mtu-offset)>=BAR_LENGTH)) {
    msg_out[offset++]='B'; // indicates a BAR follows
    append_bar(bar_number,&offset,mtu,msg_out);
  }

  // Request peers to send something interesting if they are not already
  request_wanted_content_from_peers(&offset,mtu,msg_out);

  // Only include a piece most of the them, so that we can sometimes include
  // more BARs so that the time to go through the BAR list is reduced
  if (random()&7) {
    // Announce a bundle, if any are due.
    int bundle_to_announce=find_highest_priority_bundle();
    if (debug_announce)
      fprintf(stderr,"Next bundle to announce is %d\n",bundle_to_announce);
    if (bundle_to_announce!=-1)
      announce_bundle_piece(bundle_to_announce,&offset,mtu,msg_out,
			    prefix,servald_server,credential);
    // If including a bundle piece leaves space, then try announcing another piece.
    // This basically addresses the situation where the last few bytes of a manifest
    // are included, and there is space to start sending the body.
    if ((offset+21)<mtu) {
      if (bundle_to_announce!=-1)
	announce_bundle_piece(bundle_to_announce,&offset,mtu,msg_out,
			      prefix,servald_server,credential);
    }
  }

  // Fill up spare space with BARs
  int bar_count=0;
  while (bundle_count&&(mtu-offset)>=BAR_LENGTH) {
    int bundle_number=find_highest_priority_bar();
    msg_out[offset++]='B'; // indicates a BAR follows
    append_bar(bundle_number,&offset,mtu,msg_out);
    bar_count++;
  }
  if (debug_announce) fprintf(stderr,"bar_count=%d\n",bar_count);
#else
  /* Sync by tree.
     Ask for retransmissions as required, and otherwise participate in
     synchronisation process.  Also send relevant content based on what we
     know from the sync process.
     Basically we need to iterate through the peers and pick who to respond to.
     We also need the sequence numbers to be recipient specific.
  */
  sync_by_tree_stuff_packet(&offset,mtu,msg_out,
			    prefix,servald_server,credential);
#endif

  // Increment message counter
  message_counter++;

  if (0) { 
    fprintf(stderr,"This message (hex): ");
    for(int i=0;i<offset;i++) fprintf(stderr,"%02x",msg_out[i]);
    fprintf(stderr,"\n");
  }

  radio_send_message(serialfd,msg_out,offset);

  return offset;
}
