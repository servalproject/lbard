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

#include "sync.h"
#include "lbard.h"
#include "serial.h"
#include "version.h"

#define STATUS_FILE "/tmp/lbard_status.html"

struct b {
  int order;
  long long priority;
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

  fprintf(f,"<h2>Peer list</h2>\n<table border=1 padding=2 spacing=2><tr><th>Time since last message</th></tr>\n");
  for (i=0;i<peer_count;i++)
    fprintf(f,"<tr><td>%s*</td><td>%lld</td></tr>\n",
	    peer_records[i]->sid_prefix,
	    (long long)(time(0)-peer_records[i]->last_message_time));
  fprintf(f,"</table>\n");
  fflush(f);
  
  fprintf(f,"<h2>Bundles in local store</h2>\n<table border=1 padding=2 spacing=2><tr><th>Bundle #</th><th>BID Prefix</th><th>Service</th><th>Bundle version</th><th>Bundle length</th><th>Last calculated priority</th><th># peers who don't have this bundle</th></tr>\n");
  for (n=0;n<bundle_count;n++) {
    i=order[n].order;
    fprintf(f,"<tr><td>#%d</td><td>%s</td><td>%s</td><td>%lld</td><td>%lld</td><td>0x%08llx (%lld)</td><td>%d</td></tr>\n",
	    i,
	    bundles[i].bid_hex,
	    bundles[i].service,
	    bundles[i].version,
	    bundles[i].length,
	    bundles[i].last_priority,bundles[i].last_priority,
	    bundles[i].num_peers_that_dont_have_it);
  }
  fprintf(f,"</table>\n");
  fflush(f);

  int peer;

  fprintf(f,"<h2>Bundles held by peers</h2>\n<table border=1 padding=2 spacing=2><tr><th>Peer</th><th>Bundle prefix</th><th>Bundle version</th></tr>\n");

#ifdef SYNC_BY_BAR
  for(peer=0;peer<peer_count;peer++) {
    // Don't show timed out peers
    if ((time(0)-peer_records[peer]->last_message_time)>PEER_KEEPALIVE_INTERVAL)
      continue;
    
    char *peer_prefix=peer_records[peer]->sid_prefix;
    for(i=0;i<peer_records[peer]->bundle_count;i++) {
      if (peer_records[peer]->partials[i].bid_prefix) {
	// Here is a bundle in flight
	char *bid_prefix=peer_records[peer]->bid_prefixes[i];
	long long version=peer_records[peer]->versions[i];
	fprintf(f,"<tr><td>%s*</td><td>%s*</td><td>%-18lld</td></tr>\n",
		peer_prefix?peer_prefix:"<no peer prefix>",
		bid_prefix?bid_prefix:"<no bid prefix>",version);
      }
    }
  }
#endif
  fprintf(f,"</table>\n");
  fflush(f);

  fprintf(f,"<h2>Bundles in flight</h2>\n<table border=1 padding=2 spacing=2><tr><th>Peer</th><th>Bundle prefix</th><th>Bundle version</th><th>Progress<th></tr>\n");

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
  fflush(f);

  fprintf(f,"<h2>Announced material</h2>\n<table border=1 padding=2 spacing=2><tr><th>Time</th><th>Announced content</th></tr>\n");
  long long now=gettime_ms();
  for(i=0;i<msg_count;i++) {
    fprintf(f,"<tr><td>T-%lldms</td><td>%s</td></tr>\n",
	    now-msg_times[i],msgs[i]);
    free(msgs[i]); msgs[i]=NULL;
  }
  fprintf(f,"</table>\n");
  msg_count=0;
  
  fprintf(f,"</body>\n");
  
  fclose(f);
  
  return 0;
}

time_t last_network_status_call=0;
int http_report_network_status(int socket)
{
  if ((time(0)-last_network_status_call)>1)
    {
      last_network_status_call=time(0);
      FILE *f=fopen("/tmp/networkstatus.html","w");
      if (!f) {
	char *m="HTTP/1.0 500 Couldn't create temporary file\nServer: Serval LBARD\n\nCould not create temporariy file";
	write_all(socket,m,strlen(m));
	
	return -1;
      }
      
      fprintf(f,"<html>\n<head>\n<title>Mesh Extender Radio Link Status</title>\n");
      fprintf(f,"<meta http-equiv=\"refresh\" content=\"2\" />\n</head>\n<body>\n");
      
      // Show peer reachability with indication of activity
      fprintf(f,"<h2>Mesh Extenders Reachable via Radio</h2>\n<table border=1 padding=2 spacing=2><tr><th>Mesh Extender ID</th><th>Time since last message</th></tr>\n");
      int i;
      for (i=0;i<peer_count;i++) {
	long long age=(time(0)-peer_records[i]->last_message_time);
	if (age<2) {
	  fprintf(f,"<tr><td>%s*</td><td bgcolor=\"#00ff00\">%lld sec</td></tr>\n",
		  peer_records[i]->sid_prefix,age);
	} else if (age<3) {
	  fprintf(f,"<tr><td>%s*</td><td bgcolor=\"#ffffff\">%lld sec</td></tr>\n",
		  peer_records[i]->sid_prefix,age);
	} else if (age<10) {
	  fprintf(f,"<tr><td>%s*</td><td bgcolor=\"#ffff00\">%lld sec</td></tr>\n",
		  peer_records[i]->sid_prefix,age);
	} else if (age<20) {
	  fprintf(f,"<tr><td>%s*</td><td bgcolor=\"#ff4f00\">%lld sec</td></tr>\n",
		  peer_records[i]->sid_prefix,age);
	}
      }
      fprintf(f,"</table>\n");
      
      
      // Show current transfer progress bars
      fprintf(f,"<h2>Current Bundle Transfers</h2>\n");
      fprintf(f,"<pre>\n");
      show_progress(f,1);
      fprintf(f,"</pre>\n");
      
      printf("</body>\n</html>\n");
      
      fclose(f);
    }
  return http_send_file(socket,"/tmp/networkstatus.html");     
}
