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

/*
  Update the point we intend to send from in the current bundle based on the
  request bitmap.
 */
int peer_update_send_point(int peer)
{
  // Only update if the bundle ID of the bitmap and the bundle being sent match
  if (peer_records[peer]->request_bitmap_bundle!=peer_records[peer]->tx_bundle)
    return 0;

  // Pick random piece that has yet to be received, and send that
  int candidates[256];
  int candidate_count=0;

  // But limit send point to the valid range of the bundle
  int max_bit=(cached_body_len-peer_records[peer]->request_bitmap_offset)>>6; // = /64
  // (make sure we don't leave out the last piece at the tail)
  if ((cached_body_len-peer_records[peer]->request_bitmap_offset)&63) max_bit++;
  if (max_bit>=32*8*64) max_bit=32*8*64-1; 

  // Search on even boundaries first
  int i=0; if (peer_records[peer]->request_bitmap_offset&0x40) i=1;
  for(;i<max_bit;i+=2)
    if (!(peer_records[peer]->request_bitmap[i>>3]&(1<<(i&7))))
      candidates[candidate_count++]=i;
  if (!candidate_count) {
    // No evenly aligned candidates, so include all
    for(i=0;i<max_bit;i++)
      if (!(peer_records[peer]->request_bitmap[i>>3]&(1<<(i&7))))
	candidates[candidate_count++]=i;
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
    fprintf(stderr,"BITMAP based send point = %d (candidate %d/%d = block %d)\n",
	    peer_records[peer]->tx_bundle_body_offset,
	    candidate,candidate_count,selection);
    
  }
  return 0;
}

int peer_update_request_bitmaps_due_to_transmitted_piece(int bundle_number,
							 int start_offset,
							 int bytes)
{
  fprintf(stderr,">>> %s Saw body piece [%d,%d)\n",
	 timestamp_str(),start_offset,start_offset+bytes);
  
  for(int i=0;i<MAX_PEERS;i++)
    {
      if (!peer_records[i]) continue;
      if (
	  // We have no bitmap, so start accumulating
	  (peer_records[i]->request_bitmap_bundle==-1)
	  ||
	  // We have a bitmap, but for a different bundle to the one we are sending
	  ((peer_records[i]->tx_bundle==bundle_number
	    &&peer_records[i]->request_bitmap_bundle!=peer_records[i]->tx_bundle))
	  )
	{
	  // Reset bitmap and start accumulating
	  bzero(peer_records[i]->request_bitmap,32);
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
		printf(">>> %s Marking [%d,%d) sent to peer #%d(%s*) due to transmitted piece.\n",
		       timestamp_str(),block_offset,block_offset+64,i,peer_records[i]->sid_prefix);
		if (!(peer_records[i]->request_bitmap[bit>>3]&(1<<(bit&7))))
		  fprintf(stderr,
			  "BITMAP: Setting bit %d due to transmitted piece.\n",bit);
		else
		  fprintf(stderr,
			  "BITMAP: Bit %d already set!\n",bit);
		  
		peer_records[i]->request_bitmap[bit>>3]|=(1<<(bit&7));
		bit++; bytes_remaining-=64; block_offset+=64;
	      }
	  } else {
	  printf(">>> %s NOT Marking [%d,%d) sent (start_offset<bitmap offset).\n",
		 timestamp_str(),start_offset,start_offset+bytes);
	}
      } else {
	if (peer_records[i]) {
	  if (0) printf(">>> %s NOT Marking [%d,%d) sent (no matching bitmap: %d vs %d).\n",
			timestamp_str(),start_offset,start_offset+bytes,
			peer_records[i]->request_bitmap_bundle,bundle_number);
	  if (peer_records[i]->tx_bundle==bundle_number)
	    if (0) printf(">>> %s ... but I should care, because it matches the bundle I am sending.\n",timestamp_str());
	  if (peer_records[i]->tx_bundle==-1)
	    // In fact, if we see someone sending a bundle to someone, and we don't yet know if we can send it yet, we should probably start on a speculative basis
	    printf(">>> %s ... but I could care, because I am not sending a bundle to them yet.\n",timestamp_str());
	}
      }
    }
  return 0;
}
