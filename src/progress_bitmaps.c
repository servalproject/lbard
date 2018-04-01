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

#include "sync.h"
#include "lbard.h"

int dump_progress_bitmap(FILE *f, unsigned char *b)
{
  for(int i=0;i<(32*8);i++) {
    if (b[i>>3]&(1<<(i&7)))
      fprintf(f,"."); else fprintf(f,"Y");
    //    if (((i&63)==63)&&(i!=255)) fprintf(f,"\n    ");
  }
  fprintf(f,"\n");
  return 0;
}


/*
  Generate the starting offset and bitmap of 64 byte segments that we need
  relative to that point in the payload stream.  The purpose is to provide a list
  with enough pending 64 byte segments so that all our current senders know where they
  should next send from.

  The bitmap is based on the absolute first hole in the stream that we are missing.

  The segment list is arranged in reverse order, so we start by getting the last
  piece in the segment list. If it starts at 0, then our starting point is the end
  of the first segment. If not, then our starting point is 0. We then mark the bitmap
  as requiring all pieces.  Then the segment list is retraversed, and any 64 byte
  region that we have in its entirety is marked as already held.
*/
int partial_update_request_bitmap(struct partial_bundle *p)
{
  // Get starting point
  int starting_position=0;
  // 32*8*64= 16KiB of data, enough for several seconds, even with 16 senders.
  unsigned char bitmap[32];
  bzero(&bitmap[0],32);
  struct segment_list *l=p->body_segments;
  while(l&&l->next) l=l->next;
  if (l) {
    if (!l->start_offset) starting_position=l->length;
  }

  l=p->body_segments;
  while(l) {
    if ((l->start_offset>=starting_position)
	&&(l->start_offset<=(starting_position+32*8*64))) {
      int start=l->start_offset;
      int length=l->length;
      // Ignore any first partial 
      if (start&63) {
	int trim=64-(start&63);
	start+=trim;
	length-=trim;
      }
      // Work out first block number
      int block=(start-starting_position)>>6; //  divide by 64
      // Then mark as received all those we already have
      while (length>=64&&(block<32*8)) {
	bitmap[block>>3]|=(1<<(block&7));
	block++; length-=64;
      }
    }

    l=l->next;
  }

  // Save request bitmap
  p->request_bitmap_start=starting_position;
  memcpy(p->request_bitmap,bitmap,32);

  // Now do the same for the manifest (which can have only a 16 bit long bitmap
  unsigned char manifest_bitmap[2];
  bzero(&manifest_bitmap[0],2);

  l=p->manifest_segments;
  while(l&&l->next) l=l->next;
  if (l) {
    if (!l->start_offset) starting_position=l->length;
  }

  l=p->manifest_segments;
  while(l) {
    if ((l->start_offset>=0)
	&&(l->start_offset<=1024)) {
      int start=l->start_offset;
      int length=l->length;
      // Ignore any first partial 
      if (start&63) {
	int trim=64-(start&63);
	start+=trim;
	length-=trim;
      }
      // Work out first block number
      int block=start>>6; //  divide by 64
      // Then mark as received all those we already have
      while (length>=64&&(block<16)) {
	manifest_bitmap[block>>3]|=(1<<(block&7));
	block++; length-=64;
      }

      // Mark last piece of manifest as sent for case where manifest
      // is not a multiple of 64 bytes long
      if (start+length==p->manifest_length) {
	block=p->manifest_length/64;
	manifest_bitmap[block>>3]|=(1<<(block&7));	
      }
    }

    l=l->next;
  }
  memcpy(p->request_manifest_bitmap,manifest_bitmap,2);
  
  
  return 0;
}


int progress_bitmap_translate(struct peer_state *p,int new_body_offset)
{

  // First, check if the translation requires us to discard our bitmap,
  // or whether we can keep all or part of it.
  
  // Start with an empty bitmap
  unsigned char new_request_bitmap[32];
  bzero(new_request_bitmap,32);

  int bit_delta=(new_body_offset-p->request_bitmap_offset)/64;
  
  // Copy in any bits from the pre-translation bitmap
  // We start at bit 1, not bit 0, since we assume that the reason we
  // are advancing to this point, is that the piece at this position
  // requires retransmission.
  for(int bit=1;bit<256;bit++) {
    int the_bit=0;
    int old_bit=bit+bit_delta;
    if (old_bit>=0&&old_bit<256)
      the_bit=p->request_bitmap[old_bit>>3]&(1<<(old_bit&7));
    if (the_bit)
      new_request_bitmap[bit>>3]|=(1<<(bit&7));
  }
  
  p->request_bitmap_offset=new_body_offset;
  memcpy(p->request_bitmap,new_request_bitmap,32);

  return 0;
}

/*
  Update the point we intend to send from in the current bundle based on the
  request bitmap.
 */
int peer_update_send_point(int peer)
{
  // Only update if the bundle ID of the bitmap and the bundle being sent match
  if (peer_records[peer]->request_bitmap_bundle!=peer_records[peer]->tx_bundle)
    {
      printf(">>> %s BITMAP : No updating send point because request_bitmap_bundle != tx_bundle (%d vs %d)\n",
	     timestamp_str(),peer_records[peer]->request_bitmap_bundle,peer_records[peer]->tx_bundle);
      return 0;
    }

  printf(">>> %s TX bitmap for %s* : bundle:%-2d/%-2d, m:%4d, p:%4d : ",
	 timestamp_str(),peer_records[peer]->sid_prefix,

	 peer_records[peer]->tx_bundle,
	 peer_records[peer]->request_bitmap_bundle,
	 
	 peer_records[peer]->tx_bundle_manifest_offset,
	 peer_records[peer]->request_bitmap_offset);
  // Keep all bitmaps in line, by padding front with - characters where the bitmap starts later
  for(int i=0;i<peer_records[peer]->request_bitmap_offset;i+=64) printf("-");
  dump_progress_bitmap(stdout,peer_records[peer]->request_bitmap);

  // Pick random piece that has yet to be received, and send that
#define MAX_CANDIDATES 32
  int candidates[MAX_CANDIDATES];
  int candidate_count=0;

  // But limit send point to the valid range of the bundle
  int max_bit=(cached_body_len-peer_records[peer]->request_bitmap_offset)>>6; // = /64
  // (make sure we don't leave out the last piece at the tail)
  if ((cached_body_len-peer_records[peer]->request_bitmap_offset)&63) max_bit++;
  if (max_bit>=32*8*64) max_bit=32*8*64-1; 

  // Search on even boundaries first
  int i=0; if (peer_records[peer]->request_bitmap_offset&0x40) i=1;
  for(;i<max_bit;i+=2)
    if (!(peer_records[peer]->request_bitmap[i>>3]&(1<<(i&7)))) {
      if (candidate_count<MAX_CANDIDATES) candidates[candidate_count++]=i;
    }
  if (!candidate_count) {
    // No evenly aligned candidates, so include all
    for(i=0;i<max_bit;i++)
      if (!(peer_records[peer]->request_bitmap[i>>3]&(1<<(i&7))))
      if (candidate_count<MAX_CANDIDATES) candidates[candidate_count++]=i;
  }
  
  if (!candidate_count) {
    // No candidates, so keep sending from end of region
    if (peer_records[peer]->tx_bundle_body_offset
	<=(peer_records[peer]->request_bitmap_offset+(32*8*64))) {
      peer_records[peer]->tx_bundle_body_offset
	=(peer_records[peer]->request_bitmap_offset+(32*8*64));
    }
  } else {
    int candidate=random()%candidate_count;
    int selection=candidates[candidate];
    peer_records[peer]->tx_bundle_body_offset
      =(peer_records[peer]->request_bitmap_offset+(selection*64));
    printf(">>> %s BITMAP based send point for peer #%d(%s*) = %d (candidate %d/%d = block %d)\n",
	   timestamp_str(),peer,peer_records[peer]->sid_prefix,
	   peer_records[peer]->tx_bundle_body_offset,
	   candidate,candidate_count,selection);
    
  }

  // For the manifest, we just have our simple bitmap to go through
  // XXX - We don't currently randomise
  candidate_count=0;
  for(int i=0;i<(1024/64);i++) {
    if (!(peer_records[peer]->request_manifest_bitmap[i>>3]&(1<<(i&7)))) {
      if (candidate_count<MAX_CANDIDATES)
	candidates[candidate_count++]=peer_records[peer]->tx_bundle_manifest_offset=i*64;
    }
  }
  if (!candidate_count)
    // All send, so set send point to end
    peer_records[peer]->tx_bundle_manifest_offset=1024;
  else {
    int candidate=random()%candidate_count;
    int selection=candidates[candidate];
    peer_records[peer]->tx_bundle_manifest_offset=selection;
    printf(">>> %s BITMAP based manifest send point for peer #%d(%s*) = %d (candidate %d/%d = block %d)\n",
	   timestamp_str(),peer,peer_records[peer]->sid_prefix,
	   peer_records[peer]->tx_bundle_manifest_offset,
	   candidate,candidate_count,selection>>6);
  }
  
  return 0;
}

int peer_update_request_bitmaps_due_to_transmitted_piece(int bundle_number,
							 int is_manifest,
							 int start_offset,
							 int bytes)
{
  if (!is_manifest)
    printf(">>> %s Saw body piece [%d,%d) of bundle #%d\n",
	   timestamp_str(),start_offset,start_offset+bytes,bundle_number);
  else
    printf(">>> %s Saw manifest piece [%d,%d) of bundle #%d\n",
	   timestamp_str(),start_offset,start_offset+bytes,bundle_number);
  
  for(int i=0;i<MAX_PEERS;i++)
    {
      if (!peer_records[i]) continue;
      if (
	  // We have no bitmap, so start accumulating
	  (peer_records[i]->request_bitmap_bundle==-1)
	  ||
	  // We have a bitmap, but for a different bundle to the one we are sending
	  (
	   (peer_records[i]->tx_bundle!=-1)
	   &&
	   (peer_records[i]->tx_bundle==bundle_number)
	   &&
	   (peer_records[i]->request_bitmap_bundle!=peer_records[i]->tx_bundle)
	   )
	  )
	{
	  printf(">>> %s BITMAP: Resetting progress bitmap for peer #%d(%s*): tx_bundle=%d, bundle_number=%d, request_bitmap_bundle=%d\n",
		 timestamp_str(),i,peer_records[i]->sid_prefix,
		 peer_records[i]->tx_bundle,bundle_number,
		 peer_records[i]->request_bitmap_bundle);

	  if (is_manifest) {
	    // Manifest progress is easier to update, as the bitmap is a fixed 16 bits
	    for(int j=0;j<16;j++)
	      if ((start_offset<=(64*j))
		  &&(start_offset+bytes>=(64+64*j)))
		peer_records[i]->request_manifest_bitmap[j>>3]|=1<<(j&7);
	    
	  } else {	  
	    // Reset bitmap and start accumulating
	    bzero(peer_records[i]->request_bitmap,32);
	    bzero(peer_records[i]->request_manifest_bitmap,2);
	    peer_records[i]->request_bitmap_bundle=bundle_number;
	    // The only tricky part is working out the start offset for the bitmap.
	    // If the offset of the piece is near the start, we will assume we have
	    // joined the conversation recently, and that the bitmap start is still
	    // at zero.
	    // XXX - We could lookup the bundle size to work out the size, and clamp
	    // the offset on the basis of that.
	    // XXX - If we are not currently transmitting anything to this peer, we
	    // could begin speculative transmission, since the bundle is apparently
	    // interesting to SOMEONE.  This would help to slightly reduce latency
	    // when the network is otherwise quiescent.
	    if (start_offset>16384)
	      peer_records[i]->request_bitmap_offset=start_offset;
	    else
	      peer_records[i]->request_bitmap_offset=0;
	  }
	  if (peer_records[i]->request_bitmap_bundle==bundle_number) {
	    if (start_offset>=peer_records[i]->request_bitmap_offset)
	      {
		int offset=start_offset-peer_records[i]->request_bitmap_offset;
		int block_offset=start_offset;
		int trim=offset&64;
		int bytes_remaining=bytes;
		if (trim) { offset+=64-trim; bytes_remaining-=trim; }
		int bit=offset/64;
		if (bit>=0)
		  while((bytes_remaining>=64)&&(bit<(32*8*64))) {
		    if (0)
		      printf(">>> %s Marking [%d,%d) sent to peer #%d(%s*) due to transmitted piece.\n",
			     timestamp_str(),block_offset,block_offset+64,i,peer_records[i]->sid_prefix);
		    if (!(peer_records[i]->request_bitmap[bit>>3]&(1<<(bit&7))))
		      printf(">>> %s BITMAP: Setting bit %d due to transmitted piece.\n",
			     timestamp_str(),bit);
		    else
		      printf(">>> %s BITMAP: Bit %d already set!\n",timestamp_str(),bit);
		    
		    peer_records[i]->request_bitmap[bit>>3]|=(1<<(bit&7));
		    bit++; bytes_remaining-=64; block_offset+=64;
		  }
	      } else {
	      printf(">>> %s NOT Marking [%d,%d) sent (start_offset<bitmap offset).\n",
		     timestamp_str(),start_offset,start_offset+bytes);
	    }
	  } else {
	    if (peer_records[i]) {
	      if (1) printf(">>> %s NOT Marking [%d,%d) sent to peer #%d(%s*) (no matching bitmap: %d vs %d).\n",
			    timestamp_str(),start_offset,start_offset+bytes,
			    i,peer_records[i]->sid_prefix,
			    peer_records[i]->request_bitmap_bundle,bundle_number);
	      if (peer_records[i]->tx_bundle==bundle_number)
		if (1) printf(">>> %s ... but I should care about marking it, because it matches the bundle I am sending.\n",timestamp_str());
	      if (peer_records[i]->tx_bundle==-1)
		// In fact, if we see someone sending a bundle to someone, and we don't yet know if we can send it yet, we should probably start on a speculative basis
		printf(">>> %s ... but I could care about marking it, because I am not sending a bundle to them yet.\n",timestamp_str());
	    }
	  }
	}
    }
  return 0;
}
