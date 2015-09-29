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

#include "lbard.h"


int free_peer(struct peer_state *p)
{
  if (p->sid_prefix) free(p->sid_prefix); p->sid_prefix=NULL;
  for(int i=0;i<p->bundle_count;i++) {
    if (p->bid_prefixes[i]) free(p->bid_prefixes[i]);    
  }
  for(int i=0;i<4;i++) p->sid_prefix_bin[i]=0;
  free(p->bid_prefixes); p->bid_prefixes=NULL;
  free(p->versions); p->versions=NULL;
  free(p);
  return 0;
}

int peer_note_bar(struct peer_state *p,
		  char *bid_prefix,long long version, char *recipient_prefix)
{
  int b=-1;

  if (0) {
    for(int i=0;i<p->bundle_count;i++)
      fprintf(stderr,"  bundle #%d/%d: %s* version %lld\n",
	      i,p->bundle_count,
	      p&&p->bid_prefixes&&p->bid_prefixes[i]?p->bid_prefixes[i]:"<bad>",
	      p&&p->versions&&p->versions[i]?p->versions[i]:-1);
    fprintf(stderr,"  bundle list end.\n");
  }
  
  // XXX Argh! Another linear search! Replace with something civilised
  for(int i=0;i<p->bundle_count;i++)
    if (!strcmp(p->bid_prefixes[i],bid_prefix)) {
      b=i;
      if (0) fprintf(stderr,"Peer %s* has bundle %s* version %lld (we already knew that they have version %lld)\n",
		     p->sid_prefix,bid_prefix,version,p->versions[b]);
      break;
    }
  if (b==-1) {
    // New bundle.
    if (0) fprintf(stderr,"Peer %s* has bundle %s* version %lld, which we haven't seen before\n",
	    p->sid_prefix,bid_prefix,version);
    if (p->bundle_count>=MAX_PEER_BUNDLES) {
      // BID list too full -- random replacement.
      b=random()%p->bundle_count;
      free(p->bid_prefixes[b]); p->bid_prefixes[b]=NULL;
    }
    if (p->bundle_count>=p->bundle_count_alloc) {
      // Allocate new list space
      p->bundle_count_alloc+=1000;
      p->bid_prefixes=realloc(p->bid_prefixes,sizeof(char *)*p->bundle_count_alloc);
      assert(p->bid_prefixes);
      p->versions=realloc(p->versions,sizeof(long long)*p->bundle_count_alloc);
      assert(p->versions);
    }
    b=p->bundle_count;
    if (0) fprintf(stderr,"Peer %s* bundle %s* will go in index %d (current count = %d)\n",
	    p->sid_prefix,bid_prefix,b,p->bundle_count);
    p->bid_prefixes[b]=strdup(bid_prefix);
    if (b>=p->bundle_count) p->bundle_count++;
  }
  p->versions[b]=version;
  
  return 0;
}

struct peer_state *peer_records[MAX_PEERS];
int peer_count=0;

int find_peer_by_prefix(char *peer_prefix)
{
  for(int i=0;i<peer_count;i++)
    if (!strcasecmp(peer_records[i]->sid_prefix,peer_prefix))
      return i;
  
  return -1;
}

int hex_to_val(int c)
{
  if (c>='0'&&c<'9') return c-'0';
  if (c>='A'&&c<'F') return c-'A'+10;
  if (c>='a'&&c<'f') return c-'a'+10;
  return 0;
}

int request_segment(int peer, int partial, int seg_start, int is_manifest,
		    int *offset, int mtu,unsigned char *msg_out)
{
  // Check that we have enough space
  if ((mtu-*offset)<(1+2+8+3)) return -1;

  // Request piece
  msg_out[(*offset)++]='R';

  // First 2 bytes only of peer SID. This will almost always get the benefit of
  // specifying the peer precisely, but save bytes on average
  msg_out[(*offset)++]=peer_records[peer]->sid_prefix_bin[0];
  msg_out[(*offset)++]=peer_records[peer]->sid_prefix_bin[1];

  // BID prefix
  for(int i=0;i<8;i++)
    msg_out[(*offset)++]=
      hex_to_val(peer_records[peer]->partials[partial].bid_prefix[i*2+1])
      +hex_to_val(peer_records[peer]->partials[partial].bid_prefix[i*2+0])*16;

  // Start offset of interest
  msg_out[(*offset)++]=(seg_start>>0)&0xff;
  msg_out[(*offset)++]=(seg_start>>8)&0xff;
  msg_out[(*offset)++]=((seg_start>>16)&0x7f)|(is_manifest?0x80:0x00);

  return 0;
}

int request_wanted_content_from_peers(int *offset,int mtu, unsigned char *msg_out)
{
  int peer;

  for(peer=0;peer<peer_count;peer++)
    {
      // Keep track of what we are getting from this peer, and try to finish
      // the most complete things first.
      // XXX - It would really help if we knew in advance the length of a payload
      // so that we could actually do this properly.
      // XXX - Instead for now, we just request the first missing thing we have.
      // int most_complete_partial=-1;
      // int most_complete_remaining=-1;
      // int most_complete_start=-1;
      // int most_complete_manifest_or_body=-1;
      
      // What are we fetching from this peer?
      int i;
      for(i=0;i<MAX_BUNDLES_IN_FLIGHT;i++) {
	if (peer_records[peer]->partials[i].bid_prefix) {
	  // We are receiving something from this peer, so we presume it is
	  // interesting.
	  // Our approach to requesting missing parts is simple:
	  // 1. Request missing stuff from the start, if any.
	  // 2. Else, request from the end of the first segment, so that we will tend
	  // to merge segments.
	  struct segment_list *s=peer_records[peer]->partials[i].manifest_segments;
	  if (s&&s->start_offset) {
	    // We are missing bytes at the beginning
	    return request_segment(peer,i,0,1 /* manifest */,offset,mtu,msg_out);
	  } else if (s) {
	    return request_segment(peer,i,(s->start_offset+s->length),
				   1 /* manifest */,offset,mtu,msg_out);
	  }
	  s=peer_records[peer]->partials[i].body_segments;
	  if (s&&s->start_offset) {
	    // We are missing bytes at the beginning
	    return request_segment(peer,i,0,0 /* not manifest */,offset,mtu,msg_out);
	  } else if (s) {
	    return request_segment(peer,i,(s->start_offset+s->length),
				   0 /* not manifest */,offset,mtu,msg_out);
	  }
	}
      }
    }
  return 0;
}
