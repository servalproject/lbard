/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2015-2018 Serval Project Inc., Flinders University.

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

int message_parser_4C(struct peer_state *sender,unsigned char *prefix,
		      char *servald_server, char *credential,
		      unsigned char *msg,int length)
{
  // Get instance ID of peer. We use this to note if a peer's lbard has restarted
  int offset=0;

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
  
  return offset;
}

