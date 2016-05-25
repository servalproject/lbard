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
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>

#include "sync.h"
#include "lbard.h"


int clear_partial(struct partial_bundle *p)
{
  while(p->manifest_segments) {
    struct segment_list *s=p->manifest_segments;
    p->manifest_segments=s->next;
    if (s->data) free(s->data); s->data=NULL;
    free(s);
    s=NULL;
  }
  while(p->body_segments) {
    struct segment_list *s=p->body_segments;
    p->body_segments=s->next;
    if (s->data) free(s->data); s->data=NULL;
    free(s);
    s=NULL;
  }

  bzero(p,sizeof(struct partial_bundle));
  return -1;
}

int dump_segment_list(struct segment_list *s)
{
  if (!s) return 0;
  while(s) {
    fprintf(stderr,"    [%d,%d)\n",s->start_offset,s->start_offset+s->length);
    s=s->next;
  }
  return 0;
}

int dump_partial(struct partial_bundle *p)
{
  fprintf(stderr,"Progress receiving BID=%s* version %lld:\n",
	  p->bid_prefix,p->bundle_version);
  fprintf(stderr,"  manifest is %d bytes long, and body %d bytes long.\n",
	  p->manifest_length,p->body_length);
  fprintf(stderr,"  Manifest pieces received:\n");
  dump_segment_list(p->manifest_segments);
  fprintf(stderr,"  Body pieces received:\n");
  dump_segment_list(p->body_segments);
  return 0;
}

int merge_segments(struct segment_list **s)
{
  if (!s) return -1;
  if (!(*s)) return -1;

  // Segments are sorted in descending order
  while((*s)&&(*s)->next) {
    struct segment_list *me=*s;
    struct segment_list *next=(*s)->next;
    if (me->start_offset<=(next->start_offset+next->length)) {
      // Merge this piece onto the end of the next piece
      if (debug_pieces)
	printf("Merging [%d..%d) and [%d..%d)\n",
		me->start_offset,me->start_offset+me->length,
		next->start_offset,next->start_offset+next->length);
      int extra_bytes
	=(me->start_offset+me->length)-(next->start_offset+next->length);
      int new_length=next->length+extra_bytes;
      next->data=realloc(next->data,new_length);
      assert(next->data);
      bcopy(&me->data[me->length-extra_bytes],&next->data[next->length],
	    extra_bytes);
      next->length=new_length;

      // Excise redundant segment from list
      *s=next;
      next->prev=me->prev;
      if (me->prev) me->prev->next=next;

      // Free redundant segment.
      free(me->data); me->data=NULL;
      free(me); me=NULL;
    } else 
      s=&(*s)->next;
  }
  return 0;
}
