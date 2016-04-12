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

char message_buffer[16384];
int message_buffer_size=16384;
int message_buffer_length=0;


int generate_segment_progress_string(int stream_length,
				     struct segment_list *s, char *progress)
{
  // Apply some sanity when dealing with manifests where we don't know the length yet.
  if (stream_length<1) stream_length=1024;

  int bin;
  
  for(bin=0;bin<10;bin++) progress[bin]=' ';
  

  
  while(s) {
    int bin;

    for(bin=0;bin<10;bin++) {
      int start_of_bin=stream_length*bin/10;
      int end_of_bin=stream_length*(bin+1)/10-1;
      if ((s->start_offset<=start_of_bin)
	  &&((s->start_offset+s->length-1)>=end_of_bin))
	{
	  progress[bin]='#';
	}
      else if ((s->start_offset>=start_of_bin)
	       &&((s->start_offset+s->length-1)<end_of_bin)) {
	switch(progress[bin]) {
	case ' ': progress[bin]='.'; break;
	case '.': progress[bin]=':'; break;
	case ':': progress[bin]='+'; break;
	case '#': break;
	default: progress[bin]='?'; break;
	}
      }
    }
    s=s->next;
  }
  return 0;
}

int generate_progress_string(struct partial_bundle *partial,
			     char *progress,int progress_size)
{
  if (progress_size<1) return -1;
  progress[0]=0;
  if (progress_size<80) return -1;

  // Draw up template
  snprintf(progress,80,"M          /B           ");
  
  generate_segment_progress_string(partial->manifest_length,partial->manifest_segments,
				   &progress[1]);
  generate_segment_progress_string(partial->body_length,partial->body_segments,
				   &progress[13]);


  int manifest_bytes =0,body_bytes=0;
  struct segment_list *s=partial->manifest_segments;
  while(s) {
    manifest_bytes+=s->length;
    s=s->next;
  }
  s=partial->body_segments;
  while(s) {
    body_bytes+=s->length;
    s=s->next;
  }

  snprintf(&progress[24],54," %d/%d, %d/%d",
	   manifest_bytes,partial->manifest_length,
	   body_bytes,partial->body_length
	   );

  
  return 0;
}

int show_progress()
{
  int peer,i;

  fprintf(stderr,"%s",message_buffer);
  message_buffer_length=0; message_buffer[0]=0;

  int count=0;
  
  for(peer=0;peer<peer_count;peer++) {
    char *peer_prefix=peer_records[peer]->sid_prefix;
    for(i=0;i<MAX_BUNDLES_IN_FLIGHT;i++) {
      if (peer_records[peer]->partials[i].bid_prefix) {
	// Here is a bundle in flight
	char *bid_prefix=peer_records[peer]->partials[i].bid_prefix;
	long long version=peer_records[peer]->partials[i].bundle_version;
	char progress_string[80];
	if (!count) {
	    fprintf(stderr,
		    ">> List of bundles currently being received"
		    " (%d in rhizome store)\n",
		    bundle_count);
	}
	count++;
	generate_progress_string(&peer_records[peer]->partials[i],
				 progress_string,sizeof(progress_string));
	fprintf(stderr,"   PEER:%s* %s* version %-18lld: [%s]\n",
		peer_prefix,bid_prefix,version,
		progress_string);
      }
    }
  }
  if (count) fprintf(stderr,"<< end of bundle transfer list.\n");
  return 0;
}
