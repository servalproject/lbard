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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>

#include "lbard.h"
#include "serial.h"
#include "version.h"

#define STATUS_FILE "/tmp/lbard_status.html"

struct b {
  int order;
  int priority;
};

char *msgs[1024];
long long msg_times[1024];
int msg_count=0;

int status_log(char *msg)
{
  if (msg_count<1024) {
    msgs[msg_count]=strdup(msg);
    msg_times[msg_count++]=gettime_ms();
    return 0;
  }
  return -1;
}

int compare_b(const void *a,const void *b)
{
  const struct b *aa=a;
  const struct b *bb=b;

  if (aa->priority<bb->priority) return -1;
  if (aa->priority>bb->priority) return 1;
  return 0;
}

int status_dump()
{
  FILE *f=fopen(STATUS_FILE,"w");
  if (!f) return -1;

  fprintf(f,"<HTML>\n<HEAD>lbard version %s status dump @ T=%lldms</head><body>\n",
	  VERSION_STRING,gettime_ms());
  
  struct b order[bundle_count];
  int i,n;
  
  for (i=0;i<bundle_count;i++) {
    order[i].order=i;
    order[i].priority=bundles[i].last_priority;
  }
  qsort(order,bundle_count,sizeof(struct b),compare_b);
  
  fprintf(f,"<table border=1 padding=2 spacing=2><tr><th>BID Prefix</th><th>Bundle version</th><th>Bundle length</th><th>Last calculated priority</th></tr>\n");
  for (n=0;i<bundle_count;i++) {
    i=order[n].order;
    fprintf(f,"<tr><td>%02x%02x%02x%02x%02x%02x*</td><td>%lld</td><td>%lld</td><td>0x%08llx (%lld)</td></tr>\n",
	    bundles[i].bid[0],bundles[i].bid[1],bundles[i].bid[2],
	    bundles[i].bid[3],bundles[i].bid[4],bundles[i].bid[5],
	    bundles[i].version,
	    bundles[i].length,
	    bundles[i].last_priority,bundles[i].last_priority);
  }
  fprintf(f,"</table>\n");

  fprintf(f,"<table border=1 padding=2 spacing=2><tr><th>Peer SID Prefix</th><th>Time since last message<th></tr>\n");
  for (i=0;i<peer_count;i++)
    fprintf(f,"<tr><td>%s*</td><td>%lld</td></tr>\n",
	    peer_records[i]->sid_prefix,
	    (long long)(time(0)-peer_records[i]->last_message_time));
  fprintf(f,"</table>\n");

  fprintf(f,"<table border=1 padding=2 spacing=2><tr><th>Time since last message</th></tr>\n");
  for (i=0;i<peer_count;i++)
    fprintf(f,"<tr><td>%s*</td><td>%lld</td></tr>\n",
	    peer_records[i]->sid_prefix,
	    (long long)(time(0)-peer_records[i]->last_message_time));
  fprintf(f,"</table>\n");
  
  int peer;
  fprintf(f,"<table border=1 padding=2 spacing=2><tr><th>Peer</th><th>Bundle prefix</th><th>Bundle version</th><th>Progress<th></tr>\n");

  for(peer=0;peer<peer_count;peer++) {
    char *peer_prefix=peer_records[peer]->sid_prefix;
    for(i=0;i<MAX_BUNDLES_IN_FLIGHT;i++) {
      if (peer_records[peer]->partials[i].bid_prefix) {
	// Here is a bundle in flight
	char *bid_prefix=peer_records[peer]->partials[i].bid_prefix;
	long long version=peer_records[peer]->partials[i].bundle_version;
	char progress_string[80];
	generate_progress_string(&peer_records[peer]->partials[i],
				 progress_string,sizeof(progress_string));
	fprintf(f,"<tr><td>%s*</td><td>%s*</td><td>%-18lld</td><td>[%s]</td></tr>\n",
		peer_prefix,bid_prefix,version,
		progress_string);
      }
    }
  }
  fprintf(f,"</table>\n");

    fprintf(f,"<table border=1 padding=2 spacing=2><tr><th>Time</th><th>Announced content</th></tr>\n");
  fprintf(f,"</table>\n");
  long long now=gettime_ms();
  for(i=0;i<msg_count;i++) {
    fprintf(f,"<tr><td>T-%lldms</td><td>%s</td></tr>\n",
	    now-msg_times[i],msgs[i]);
    free(msgs[i]); msgs[i]=NULL;
  }
  msg_count=0;
  
  fprintf(f,"</body>\n");
  
  fclose(f);
  
  return 0;
}
