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
#include <sys/time.h>

#include "sync.h"
#include "lbard.h"

extern char *my_sid_hex;

int saw_length(char *peer_prefix,char *bid_prefix,long long version,
	       int body_length)
{
  // Note length of payload for this bundle, if we don't already know it
  int peer=find_peer_by_prefix(peer_prefix);
  if (peer<0) return -1;

  int i;
  int spare_record=random()%MAX_BUNDLES_IN_FLIGHT;
  for(i=0;i<MAX_BUNDLES_IN_FLIGHT;i++) {
    if (!partials[i].bid_prefix) {
      if (spare_record==-1) spare_record=i;
    } else {
      if (!strcasecmp(partials[i].bid_prefix,bid_prefix))
	if (partials[i].bundle_version==version)
	  {
	    partials[i].body_length=body_length;
	    return 0;
	  }
    }
  }
  return -1;
}

int saw_piece(char *peer_prefix,int for_me,
	      char *bid_prefix, unsigned char *bid_prefix_bin,
	      long long version,
	      long long piece_offset,int piece_bytes,int is_end_piece,
	      int is_manifest_piece,unsigned char *piece,

	      char *prefix, char *servald_server, char *credential)
{
  int next_byte_would_be_useful=0;
  int new_bytes_in_piece=0;
  
  int peer=find_peer_by_prefix(peer_prefix);
  if (peer<0) return -1;

  if (debug_pieces) printf("Saw a piece of BID=%s* from SID=%s*\n",
			    bid_prefix,peer_prefix);
  
  int bundle_number=-1;

  // Send an ack immediately if we already have this bundle (or newer), so that the
  // sender knows that they can start sending something else.
  // This in effect provides a positive ACK for reception of a new bundle.
  // XXX - If the sender depends on the ACK to start sending the next bundle, then
  // an adversary could purposely refuse to acknowledge bundles (that it might have
  // introduced for this special purpose) addressed to itself, so that the priority
  // scheme gets stuck trying to send these bundles to them forever.
  if (for_me) {
    if (sync_is_bundle_recently_received(bid_prefix,version)) {
      // We have this version already: mark it for announcement to sender,
      // and then return immediately.
      fprintf(stderr,
	      "We recently received %s* version %lld - ignoring piece.\n",
	      bid_prefix,version);
      sync_tell_peer_we_have_bundle_by_id(peer,bid_prefix_bin,version);
      return 0;      
    }
  }
  for(int i=0;i<bundle_count;i++) {
    if (!strncasecmp(bid_prefix,bundles[i].bid_hex,strlen(bid_prefix))) {
      if (debug_pieces) printf("We have version %lld of BID=%s*.  %s is offering us version %lld\n",
	      bundles[i].version,bid_prefix,peer_prefix,version);
      if (version<=bundles[i].version) {
	// We have this version already: mark it for announcement to sender,
	// and then return immediately.
#ifdef SYNC_BY_BAR
	bundles[i].announce_bar_now=1;
#endif
	if (for_me) {
	  fprintf(stderr,"We already have %s* version %lld - ignoring piece.\n",
		  bid_prefix,version);
	  sync_tell_peer_we_have_this_bundle(peer,i);
	}
	return 0;
      } else {
	// We have an older version.
	// Remember the bundle number so that we can pre-fetch the body we have
	// for incremental journal transfers
	if (version<0x100000000LL) {
	  bundle_number=i;
	}	
      }
    }
  }

  int i;
  int spare_record=random()%MAX_BUNDLES_IN_FLIGHT;
  for(i=0;i<MAX_BUNDLES_IN_FLIGHT;i++) {
    if (!partials[i].bid_prefix) {
      if (spare_record==-1) spare_record=i;
    } else {
      if (!strcasecmp(partials[i].bid_prefix,bid_prefix))
	{
	  if (debug_pieces) printf("Saw another piece for BID=%s* from SID=%s: ",
			 bid_prefix,peer_prefix);
	  if (debug_pieces) printf("[%lld..%lld)\n",
			 piece_offset,piece_offset+piece_bytes);

	  break;
	}
      else {
	if (debug_pieces) {
	  printf("  this isn't the partial we are looking for.\n");
	  printf("  piece is of %s*, but slot #%d has %s*\n",
		 bid_prefix,i,
		 partials[i].bid_prefix);
	}
      }
    }
  }

  if (debug_pieces)
    printf("Saw a piece of interesting bundle BID=%s*/%lld from SID=%s\n",
	    bid_prefix,version, peer_prefix);
  
  if (i==MAX_BUNDLES_IN_FLIGHT) {
    if (spare_record>0) i=spare_record;
    if (debug_pieces)
      printf("Didn't find bundle in partials for this peer. first spare slot =%d\n",spare_record);
    // Didn't find bundle in the progress list.
    // Abort one of the ones in the list at random, and replace, unless there is
    // a spare record slot to use.
    if (spare_record==-1) {
      i=random()%MAX_BUNDLES_IN_FLIGHT;
      clear_partial(&partials[i]);
    } else {
      i=spare_record;
      // Clear it just to make sure.
      clear_partial(&partials[i]);
    }
    if (debug_pieces)
      printf("@@@   Using slot %d\n",i);

    // Now prepare the partial record
    partials[i].bid_prefix=strdup(bid_prefix);
    partials[i].bundle_version=version;
    partials[i].manifest_length=-1;
    partials[i].body_length=-1;
  }

  partial_update_recent_senders(&partials[i],peer_prefix);
  
  int piece_end=piece_offset+piece_bytes;

  // Note stream length if this is an end piece or journal bundle
  if (is_end_piece) {
    if (is_manifest_piece)
      partials[i].manifest_length=piece_end;
    else
      partials[i].body_length=piece_end;
  }
  if (version<0x100000000LL) {
    // Journal bundle, so body_length = version
    partials[i].body_length=version;
  }

  if ((bundle_number>-1)
      &&(!partials[i].body_segments)) {
    // This is a bundle that for which we already have a previous version, and
    // for which we as yet have no body segments.  So fetch from Rhizome the content
    // that we do have, and prepopulate the body segment.
    fprintf(stderr,"%s:%d:My SID as hex is %s\n",__FILE__,__LINE__,my_sid_hex);
    if (!prime_bundle_cache(bundle_number,my_sid_hex,servald_server,credential)) {
      struct segment_list *s=calloc(1,sizeof(struct segment_list));
      assert(s);
      s->data=malloc(cached_body_len);
      assert(s->data);
      bcopy(cached_body,s->data,cached_body_len);
      s->start_offset=0;
      s->length=cached_body_len;
      partials[i].body_segments=s;
      if (debug_pieces)
	printf("Preloaded %d bytes from old version of journal bundle.\n",
		cached_body_len);
    } else {
      if (debug_pieces)
	printf("Failed to preload bytes from old version of journal bundle. XFER will likely fail due to far end thinking it can skip the bytes we already have, so ignoring current piece.\n");
      return -1;
    }
  }

  // Now we have the right partial, we need to look for the right segment to add this
  // piece to, if any.
  struct segment_list **s;
  if (is_manifest_piece) s=&partials[i].manifest_segments;
  else s=&partials[i].body_segments;

  /*
    The segment lists are maintained in reverse order, since pieces will generally
    arrive in ascending address order.
  */
  int segment_start;
  int segment_end;
  while(1) {
    if (*s) {
      segment_start=(*s)->start_offset;
      segment_end=segment_start+(*s)->length;
    } else {
      segment_start=-1; segment_end=-1;
    }
    
    if ((!(*s))||(segment_end<piece_offset)) {
      // Create a new segment before the current one
      new_bytes_in_piece=piece_bytes;

      if (debug_pieces) printf("Inserting piece [%lld..%lld) before [%d..%d)\n",
		     piece_offset,piece_offset+piece_bytes,
		     segment_start,segment_end);

      struct segment_list *ns=calloc(1,sizeof(struct segment_list));
      assert(ns);

      // Link into the list
      ns->next=*s;
      if (*s) ns->prev=(*s)->prev; else ns->prev=NULL;
      if (*s) (*s)->prev=ns;
      *s=ns;

      // Set start and ends and allocate and copy in piece data
      ns->start_offset=piece_offset;
      ns->length=piece_bytes;
      ns->data=malloc(piece_bytes);
      bcopy(piece,ns->data,piece_bytes);

      // This is data that is new, and the next byte would also be new, so
      // no need to tell the peer to change where they are sending from in the bundle.
      next_byte_would_be_useful=1;
      
      break;
    } else if ((segment_start<=piece_offset)&&(segment_end>=piece_end)) {
      // Piece fits entirely within a current segment, i.e., is not new data
      new_bytes_in_piece=0;
      break;
    } else if (piece_end<segment_start) {
      // Piece ends before this segment starts, so proceed down the list further.
      if (debug_pieces)
	printf("Piece [%lld..%lld) comes before [%d..%d)\n",
		piece_offset,piece_offset+piece_bytes,
		segment_start,segment_end);
      
      s=&(*s)->next;
    } else {
      // Segment should abutt or overlap with new piece.
      // Pieces can be different sizes, so it is possible to extend both directions
      // at once.

      // New piece and existing segment should overlap or adjoin.  Otherwise abort.
      int piece_start=piece_offset;
      assert( ((segment_start>=piece_start)&&(segment_start<=piece_end))
	      ||((segment_end>=piece_start)&&(segment_end<=piece_end))
	      );      
      if (0)
      {
	message_buffer_length+=
	  snprintf(&message_buffer[message_buffer_length],
		   message_buffer_size-message_buffer_length,
		   "Received %s",bid_prefix);
	message_buffer_length+=
	  snprintf(&message_buffer[message_buffer_length],
		   message_buffer_size-message_buffer_length,
		   "* version %lld %s segment [%d,%d)\n",
		   version,
		   is_manifest_piece?"manifest":"payload",
		   piece_start,piece_start+piece_bytes);
      }
            
      if (piece_start<segment_start) {
	// Need to stick bytes on the start
	int extra_bytes=segment_start-piece_start;
	int new_length=(*s)->length+extra_bytes;
	unsigned char *d=malloc(new_length);
        assert(d);
	bcopy(piece,d,extra_bytes);
	bcopy((*s)->data,&d[extra_bytes],(*s)->length);
	(*s)->start_offset=piece_start;
	(*s)->length=new_length;
	free((*s)->data); (*s)->data=d;
	new_bytes_in_piece+=extra_bytes;
      }
      if (piece_end>segment_end) {
	// Need to sick bytes on the end
	int extra_bytes=piece_end-segment_end;
	int new_length=(*s)->length+extra_bytes;
	(*s)->data=realloc((*s)->data,new_length);
        assert((*s)->data);
	bcopy(&piece[piece_bytes-extra_bytes],&(*s)->data[(*s)->length],
	      extra_bytes);
	(*s)->length=new_length;
	new_bytes_in_piece+=extra_bytes;

	// We have extended beyond the end, so the next byte is most likely
	// useful, unless it happens to extend to the start of the next segment.
	// XXX - We are ignoring that case for now, as worst it will cause only
	// one wasted packet.  But it would be nice to detect this situation.
	next_byte_would_be_useful=1;
      }
      
      break;
    } 
  }

  merge_segments(&partials[i].manifest_segments);
  merge_segments(&partials[i].body_segments);
  partial_update_request_bitmap(&partials[i]);
  fprintf(stderr,"(Piece was [%lld,%lld)\n",piece_offset,piece_offset+piece_bytes);

  partials[i].recent_bytes += piece_bytes;
  
  // Check if we have the whole bundle now
  // XXX - this breaks when we have nothing about the bundle, because then we think the length is zero, so we think we have it all, when really we have none.
  if (partials[i].manifest_segments
      &&partials[i].body_segments
      &&(!partials[i].manifest_segments->next)
      &&(!partials[i].body_segments->next)
      &&(partials[i].manifest_segments->start_offset==0)
      &&(partials[i].body_segments->start_offset==0)
      &&(partials[i].manifest_segments->length
	 ==partials[i].manifest_length)
      &&(partials[i].body_segments->length
	 ==partials[i].body_length))
    {
      // We have a single segment for body and manifest that span the complete
      // size.
      printf(">>> %s We have the entire bundle %s*/%lld now.\n",
	     timestamp_str(),bid_prefix,version);

      // First, reconstitute the manifest from the binary encoded format
      unsigned char manifest[1024];
      int manifest_len;

      int insert_result=-999;
      
      if (!manifest_binary_to_text
	  (partials[i].manifest_segments->data,
	   partials[i].manifest_length,
	   manifest,&manifest_len)) {

	// Display decompressed manifest
	dump_bytes("Decompressed Manifest",manifest,manifest_len);
	
	insert_result=
	  rhizome_update_bundle(manifest,manifest_len,
				partials[i].body_segments->data,
				partials[i].body_length,
				servald_server,credential);

	if (debug_bundlelog) {
	  // Write details of bundle to a log file for monitoring
	  // This is used for rhizome velocity experiments.  For that purpose,
	  // we like to know the name of the bundle we are looking for, so we include
	  // it in the message.
	  FILE *bundlelogfile=fopen("bundles_received.log","a");
	  if (bundlelogfile) {
	    char bid[1024];
	    char filename[1024];
	    char message[1024];
	    char filesize[1024];
	    manifest_get_field(manifest,manifest_len,"name",filename);
	    manifest_get_field(manifest,manifest_len,"id",bid);
	    manifest_get_field(manifest,manifest_len,"filesize",filesize);
	    snprintf(message,1024,"T+%lldms:%s:%s:%s:%s\n",
		     (long long)(gettime_ms()-start_time),
		     my_sid_hex,bid,filename,filesize);
	    fprintf(bundlelogfile,"%s",message);
	    fclose(bundlelogfile);
	  }
	}

	// Take note of the bundle, so that we can tell any peer who is trying to
	// send it to us, that we have recently received it.  This is irrespective
	// of whether it inserted correctly. The reasoning behind this, is that we
	// don't want a peer to get stuck sending the same bundle over and over
	// again.  It is better to send something else, and work through all the
	// bundles that need sending first. Then after that, if we restart our sync
	// process periodically, we will catch any straglers. It still isn't perfect,
	// but it's a start.
	sync_remember_recently_received_bundle
	  (partials[i].bid_prefix,
	   partials[i].bundle_version);
      } else {
	printf(">>> %s Could not decompress binary manifest.  Not inserting\n",
	       timestamp_str());
	// This will cause us to try to receive the entire bundle again, not just the
	// manifest.
	// XXX - Decompress manifest as soon as we have it to catch this problem
	// earlier. 
      }
      if (insert_result) {
	// Failed to insert, so mark this bundle for deprioritisation, so that we
	// don't just keep asking for it.
	fprintf(stderr,"Failed to insert bundle %s*/%lld (result=%d)\n",
		partials[i].bid_prefix,
		partials[i].bundle_version,insert_result);
	dump_bytes("manifest",manifest,manifest_len);
	dump_bytes("payload",
		   partials[i].body_segments->data,
		   partials[i].body_length);

	char bid[32*2+1];
	if (!manifest_extract_bid(partials[i].manifest_segments->data,
				  bid)) {
#ifdef SYNC_BY_BAR
	  int bundle=bid_to_peer_bundle_index(peer,bid);
	  if (peer_records[peer]->insert_failures[bundle]<255)
	    peer_records[peer]->insert_failures[bundle]++;
#endif
	}
      } else {
	// Insert succeeded, so clear any failure deprioritisation (although it
	// shouldn't matter).
	char bid[32*2+1];
	if (!manifest_extract_bid(partials[i].manifest_segments->data,
				  bid)) {
#ifdef SYNC_BY_BAR
	  int bundle=bid_to_peer_bundle_index(peer,bid);
	  peer_records[peer]->insert_failures[bundle]=0;
#endif
	}
	progress_log_bundle_receipt(partials[i].bid_prefix,
				    partials[i].bundle_version);
      }

      // Tell peer we have the whole thing now.
      // (next_byte_would_be_useful is asserted so that we don't send two
      // reports).
      next_byte_would_be_useful=1;
      sync_tell_peer_we_have_the_bundle_of_this_partial(peer,i);
      
      // Now release this partial.
      clear_partial(&partials[i]);
    }
  else {
    // To deal with multiple senders that are providing us with pieces in lock-step,
    // we want to be able to redirect them to send from different positions in the bundle.
    // XXX - Does this mean that we will never have to deal with next_byte_would_be_useful==0 ?
    if (!option_flags&FLAG_NO_BITMAP_PROGRESS) {
      if (!new_bytes_in_piece)
	sync_schedule_progress_report(peer,i,1 /* random jump */);
      else if (!next_byte_would_be_useful)
	sync_schedule_progress_report(peer,i,0 /* send from first required byte */);
    } else {
      fprintf(stderr,"Sending BITMAP\n");
      sync_schedule_progress_report_bitmap(peer,i);
    }
  }
  
  return 0;
}

extern int my_time_stratum;

int saw_message(unsigned char *msg,int len,char *my_sid,
		char *prefix, char *servald_server,char *credential)
{
  /*
    Parse message and act on it.    
  */
  
  // All valid messages must be at least 8 bytes long.
  if (len<8) return -1;
  char peer_prefix[6*2+1];
  snprintf(peer_prefix,6*2+1,"%02x%02x%02x%02x%02x%02x",
	   msg[0],msg[1],msg[2],msg[3],msg[4],msg[5]);
  int msg_number=msg[6]+256*(msg[7]&0x7f);
  int is_retransmission=msg[7]&0x80;

  // Ignore messages from ourselves
  if (!bcmp(msg,my_sid,6)) return -1;
  
  if (debug_pieces) {
    printf("Decoding message #%d from %s*, length = %d:\n",
	    msg_number,peer_prefix,len);
  }

  int offset=8; 

  char bid_prefix[8*2+1];
  long long version;
  int size_byte;
  char recipient_prefix[4*2+1];
  unsigned int offset_compound;
  long long piece_offset;
  int piece_bytes;
  int piece_is_manifest;
  int above_1mb;
  int is_end_piece;
  int for_me=0;

  int peer_index=-1;
  
  // Find or create peer structure for this.
  struct peer_state *p=NULL;
  for(int i=0;i<peer_count;i++) {
    if (!strcasecmp(peer_records[i]->sid_prefix,peer_prefix)) {
      p=peer_records[i]; peer_index=i; break;
    }
  }

  if (!p) {
    p=calloc(1,sizeof(struct peer_state));
    for(int i=0;i<4;i++) p->sid_prefix_bin[i]=msg[i];
    p->sid_prefix=strdup(peer_prefix);
    p->last_message_number=-1;
    p->tx_bundle=-1;
    printf("Registering peer %s*\n",p->sid_prefix);
    if (peer_count<MAX_PEERS) {
      peer_records[peer_count++]=p;      
    } else {
      // Peer table full.  Do random replacement.
      peer_index=random()%MAX_PEERS;
      free_peer(peer_records[peer_index]);
      peer_records[peer_index]=p;
    }
  }
  
  // Update time stamp and most recent message from peer
  p->last_message_time=time(0);
  if (!is_retransmission) p->last_message_number=msg_number;
  
  while(offset<len) {
    if (debug_pieces||debug_message_pieces) {
      printf(
	      "Saw message section with type '%c' (0x%02x) @ offset $%02x, len=%d\n",
	      msg[offset],msg[offset],offset,len);
      fflush(stderr);
    }
    switch(msg[offset]) {
    case 'A': case 'a':
      /* Acknowledgement of progress of bundle transfer */
      sync_parse_ack(p,&msg[offset],
		     prefix,servald_server,credential);
      offset+=17;
      break;
    case 'B':
      offset++;
      if (len-offset<BAR_LENGTH) {
	fprintf(stderr,"Ignoring runt BAR (len=%d instead of %d)\n",
		len-offset,BAR_LENGTH);
	return -2;
      }
      // BAR announcement
      unsigned char *bid_prefix_bin=&msg[offset];
      snprintf(bid_prefix,8*2+1,"%02X%02X%02X%02X%02X%02X%02X%02X",
	       msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3],
	       msg[offset+4],msg[offset+5],msg[offset+6],msg[offset+7]);
      offset+=8;
      version=0;
      for(int i=0;i<8;i++) version|=((long long)msg[offset+i])<<(i*8LL);
      offset+=8;
      snprintf(recipient_prefix,4*2+1,"%02x%02x%02x%02x",
	       msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3]);
      offset+=4;
      size_byte=msg[offset];
      offset+=1;
#ifdef SYNC_BY_BAR
      if (debug_pieces)
	printf(
		"Saw a BAR from %s*: %s* version %lld size byte 0x%02x"
		" (we know of %d bundles held by that peer)\n",
		p->sid_prefix,bid_prefix,version,size_byte,p->bundle_count);
#endif
      if (monitor_mode)
      {
	char sender_prefix[128];
	char monitor_log_buf[1024];
	sprintf(sender_prefix,"%s*",p->sid_prefix);
	snprintf(monitor_log_buf,sizeof(monitor_log_buf),
		 "BAR: BID=%s*, version 0x%010llx,"
		 " %smeshms payload has %lld--%lld bytes,"
#ifdef SYNC_BY_BAR
		 " (%d unique)"
#endif
		 ,
		 bid_prefix,version,
		 (size_byte&0x80)?"non-":"",
		 (size_byte&0x7f)?(size_byte_to_length((size_byte&0x7f)-1)):0,
		 size_byte_to_length((size_byte&0x7f))-1
#ifdef SYNC_BY_BAR
		 ,p->bundle_count
#endif
		 );	
	
	monitor_log(sender_prefix,NULL,monitor_log_buf);
      }

#ifdef SYNC_BY_BAR
      peer_note_bar(p,bid_prefix,version,recipient_prefix,size_byte);
#else
      int bundle=lookup_bundle_by_prefix_bin_and_version_or_older(bid_prefix_bin,version);
      if (bundle>-1) {
	printf("T+%lldms : SYNC FIN: %s* has finished receiving"
		" %s version %lld (bundle #%d)\n",
		gettime_ms()-start_time,p?p->sid_prefix:"<null>",bid_prefix,
		version,bundle);

	sync_dequeue_bundle(p,bundle);
      } else {
        printf("T+%lldms : SYNC FIN: %s* has finished receiving"
	  " %s (%02X...) version %lld (NO SUCH BUNDLE!)\n",
	  gettime_ms()-start_time,p?p->sid_prefix:"<null>",
	  bid_prefix,bid_prefix_bin[0],version);
      }

#endif
      break;
    case 'G':
      // Get instance ID of peer. We use this to note if a peer's lbard has restarted
      offset++;
      {
	unsigned int peer_instance_id=0;
	for(int i=0;i<4;i++) peer_instance_id|=(msg[offset++]<<(i*8));
	if (!p->instance_id) p->instance_id=peer_instance_id;
	if (p->instance_id!=peer_instance_id) {
	  // Peer's instance ID has changed: Forget all knowledge of the peer and
	  // return (ignoring the rest of the packet).
#ifndef SYNC_BY_BAR
	  free_peer(peer_records[peer_index]);
	  p=calloc(1,sizeof(struct peer_state));
	  for(int i=0;i<4;i++) p->sid_prefix_bin[i]=msg[i];
	  p->sid_prefix=strdup(peer_prefix);
	  p->last_message_number=-1;
	  p->tx_bundle=-1;
	  p->instance_id=peer_instance_id;
	  printf("Peer %s* has restarted -- discarding stale knowledge of its state.\n",p->sid_prefix);
	  peer_records[peer_index]=p;
#endif
	}
      }
      break;
    case 'L':
      // Length of bundle announcement for receivers
      offset++;
      if (len-offset<(1+8+8+4)) return -3;
      int bid_prefix_offset=offset;
      snprintf(bid_prefix,8*2+1,"%02x%02x%02x%02x%02x%02x%02x%02x",
	       msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3],
	       msg[offset+4],msg[offset+5],msg[offset+6],msg[offset+7]);
      offset+=8;
      version=0;
      for(int i=0;i<8;i++) version|=((long long)msg[offset+i])<<(i*8LL);
      offset+=8;
      offset_compound=0;
      for(int i=0;i<4;i++) offset_compound|=((long long)msg[offset+i])<<(i*8LL);
      offset+=4;

      if (monitor_mode)
      {
	char sender_prefix[128];
	char monitor_log_buf[1024];
	sprintf(sender_prefix,"%s*",p->sid_prefix);
	char bid_prefix[128];
	bytes_to_prefix(&msg[bid_prefix_offset],bid_prefix);
	snprintf(monitor_log_buf,sizeof(monitor_log_buf),
		 "Payload length: BID=%s*, version 0x%010llx, length = %d bytes",
		 bid_prefix,version,offset_compound);
	
	monitor_log(sender_prefix,NULL,monitor_log_buf);
      }

      saw_length(peer_prefix,bid_prefix,version,offset_compound);
      break;
    case 'M':
      /* Acknowledgement of progress of bundle transfer */
      sync_parse_progress_bitmap(p,msg,&offset);
      break;
    case 'P': case 'p': case 'Q': case 'q':
      // Skip header character
      above_1mb=0;
      is_end_piece=0;
      if (!(msg[offset]&0x20)) above_1mb=1;
      if (!(msg[offset]&0x01)) is_end_piece=1;
      offset++;
      
      // Work out from target SID, if this is intended for us
      if ((my_sid[0]!=msg[offset])||(my_sid[1]!=msg[offset+1])) for_me=0;
      else for_me=1;
      offset+=2;
      
      if (len-offset<(1+8+8+4+1)) return -3;
      bid_prefix_bin=&msg[offset];
      snprintf(bid_prefix,8*2+1,"%02x%02x%02x%02x%02x%02x%02x%02x",
	       msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3],
	       msg[offset+4],msg[offset+5],msg[offset+6],msg[offset+7]);
      offset+=8;
      version=0;
      for(int i=0;i<8;i++) version|=((long long)msg[offset+i])<<(i*8LL);
      offset+=8;
      offset_compound=0;
      for(int i=0;i<6;i++) offset_compound|=((long long)msg[offset+i])<<(i*8LL);
      offset+=4;
      if (above_1mb) offset+=2; else offset_compound&=0xffffffff;
      piece_offset=(offset_compound&0xfffff)|((offset_compound>>12LL)&0xfff00000LL);
      piece_bytes=(offset_compound>>20)&0x7ff;
      piece_is_manifest=offset_compound&0x80000000;

      if (monitor_mode)
	{
	  char sender_prefix[128];
	  char monitor_log_buf[1024];
	  sprintf(sender_prefix,"%s*",p->sid_prefix);
	  snprintf(monitor_log_buf,sizeof(monitor_log_buf),
		   "Piece of bundle: BID=%s*, [%lld--%lld) of %s.%s",
		   bid_prefix,
		   piece_offset,piece_offset+piece_bytes-1,
		   piece_is_manifest?"manifest":"payload",
		   is_end_piece?" This is the last piece of that.":""
		   );
	  
	  monitor_log(sender_prefix,NULL,monitor_log_buf);
	}
            
      saw_piece(peer_prefix,for_me,
		bid_prefix,bid_prefix_bin,
		version,piece_offset,piece_bytes,is_end_piece,
		piece_is_manifest,&msg[offset],
		prefix, servald_server,credential);

      if (piece_bytes>0) offset+=piece_bytes;

      break;
    case 'R':
      // Request for a segment
      {
	char target_sid[4+1+1];
	char bid_prefix[8*2+1+1];
	int bundle_offset=0;
	int is_manifest=0;
	offset++;
	snprintf(target_sid,5,"%02x%02x",msg[offset],msg[offset+1]);
	offset+=2;
	snprintf(bid_prefix,17,"%02x%02x%02x%02x%02x%02x%02x%02x",
		 msg[offset+0],msg[offset+1],msg[offset+2],msg[offset+3],
		 msg[offset+4],msg[offset+5],msg[offset+6],msg[offset+7]);
	offset+=8;
	bundle_offset|=msg[offset++];
	bundle_offset|=msg[offset++]<<8;
	bundle_offset|=msg[offset++]<<16;
	// We can only request segments upto 8MB point in a bundle via this transport!
	// XXX here be dragons
	if (bundle_offset&0x800000) is_manifest=1;
	bundle_offset&=0x7fffff;
	
	if (debug_pull) {
	  printf("Saw request from SID=%s* BID=%s @ %c%d addressed to SID=%s*\n",
		  peer_prefix,bid_prefix,is_manifest?'M':'B',bundle_offset,
		  target_sid);
	}
	{
	  char status_msg[1024];
	  snprintf(status_msg,1024,"Saw request from SID=%s* BID=%s @ %c%d addressed to SID=%s*\n",
		   peer_prefix,bid_prefix,is_manifest?'M':'B',bundle_offset,
		   target_sid);
	  status_log(status_msg);
	}

      if (monitor_mode)
	{
	  char sender_prefix[128];
	  char monitor_log_buf[1024];
	  sprintf(sender_prefix,"%s*",p->sid_prefix);
	  snprintf(monitor_log_buf,sizeof(monitor_log_buf),
		   "Request for BID=%s*, beginning at offset %d of %s.",
		   bid_prefix,bundle_offset,is_manifest?"manifest":"payload");
	  
	  monitor_log(sender_prefix,NULL,monitor_log_buf);
	}

#ifdef SYNC_BY_BAR
	// Are we the target SID?
	if (!strncasecmp(my_sid,target_sid,4)) {
	  if (debug_pull) printf("  -> request is for us.\n");
	  // Yes, it is addressed to us.
	  // See if we have this bundle, and if so, set the appropriate stream offset
	  // and mark the bundle as requested
	  // XXX linear search!
	  for(int i=0;i<bundle_count;i++) {
	    if (!strncasecmp(bid_prefix,bundles[i].bid,16)) {
	      if (debug_pull) printf("  -> found the bundle.\n");
	      bundles[i].transmit_now=time(0)+TRANSMIT_NOW_TIMEOUT;
	      if (debug_announce) {
		printf("*** Setting transmit_now flag on %s*\n",
			bundles[i].bid);
	      }

	      // When adjusting the offset, don't adjust it if we are going to reach
	      // that point within a few hundred bytes, as it won't save any time, and
	      // it might just cause confusion and delay because of the latency of us
	      // receiving the message and responding to it.
	      if (is_manifest) {
		if ((bundle_offset<bundles[i].last_manifest_offset_announced)
		    ||((bundle_offset-bundles[i].last_manifest_offset_announced)>500)) {
		  bundles[i].last_manifest_offset_announced=bundle_offset;
		  if (debug_pull) printf("  -> setting manifest announcement offset to %d.\n",bundle_offset);
		}
	      } else {
		if ((bundle_offset<bundles[i].last_offset_announced)
		    ||((bundle_offset-bundles[i].last_offset_announced)>500)) {
		bundles[i].last_offset_announced=bundle_offset;
		if (debug_pull) printf("  -> setting body announcement offset to %d.\n",bundle_offset);
		}
	      }
	    }
	  }
	}
#endif
      }
      break;
    case 'S':
      // Sync-tree synchronisation message

      // process the message
      sync_tree_receive_message(p,&msg[offset]);

      // Skip over the message
      if (msg[offset+1]) offset+=msg[offset+1];
      // Zero field length is clearly an error, so abort
      else {
	if (monitor_mode)
	  {
	    char sender_prefix[128];
	    char monitor_log_buf[1024];
	    sprintf(sender_prefix,"%s*",p->sid_prefix);
	    
	    snprintf(monitor_log_buf,sizeof(monitor_log_buf),
		     "S field with zero length at radio packet offset %d",
		     offset);
	    
	    monitor_log(sender_prefix,NULL,monitor_log_buf);
	  }      
	return -1;
      }
      break;
    case 'T':
      // Time stamp
      {
	offset++;
	int stratum=msg[offset++];
	struct timeval tv;
	bzero(&tv,sizeof (struct timeval));
	for(int i=0;i<8;i++) tv.tv_sec|=msg[offset++]<<(i*8);
	for(int i=0;i<3;i++) tv.tv_usec|=msg[offset++]<<(i*8);
	/* XXX - We don't do any clever NTP-style time correction here.
	   The result will be only approximate, probably accurate to only
	   ~10ms - 100ms per stratum, and always running earlier and earlier
	   with each stratum, as we fail to correct the received time for 
	   transmission duration.
	   We can at least try to fix this a little:
	   1. UHF radio serial speed = 230400bps = 23040cps.
	   2. Packets are typically ~250 bytes long.
	   3. Serial TX speed to radio is thus ~10.8ms
	   4. UHF Radio air speed is 128000bps.
	   5. Radio TX time is thus 250*8/128000= ~15.6ms
	   6. Total minimum delay is thus ~26.4ms

	   Thus we will make this simple correction of adding 26.4ms.

	   The next challenge is if we have multiple sources with the same stratum
	   giving us the time.  In that case, we need a way to choose a winner, since
	   we are not implementing fancy NTP-style time integration algorithms. The
	   trick is to get something simple, that stops clocks jumping backwards and
	   forwards allover the shop.  A really simple approach is to have a timeout
	   when updating the time, and ignore updates from the same time stratum for
	   the next several minutes.  We should also decay our stratum if we have not
	   heard from an up-stream clock lately, so that we always converge on the
	   freshest clock.  In fact, we can use the slow decay to implement this
	   quasi-stability that we seek.
	 */	
	tv.tv_usec+=26400;

	char sender_prefix[128];	
	sprintf(sender_prefix,"%s*",p->sid_prefix);

	saw_timestamp(sender_prefix,stratum,&tv);
      }
      break;
    default:
      // invalid message field.
	if (monitor_mode)
	  {
	    char sender_prefix[128];
	    char monitor_log_buf[1024];
	    sprintf(sender_prefix,"%s*",p->sid_prefix);

	    snprintf(monitor_log_buf,sizeof(monitor_log_buf),
		     "Illegal message field 0x%02X at radio packet offset %d",
		     msg[offset],offset);
	    
	    monitor_log(sender_prefix,NULL,monitor_log_buf);
	  }      
      return -1;
    }
  }
  return 0;
}
