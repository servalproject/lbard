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


int message_parser_47(struct peer_state *sender,unsigned char *prefix,
		      char *servald_server, char *credential,
		      unsigned char *msg,int length)
{
  // Get instance ID of peer. We use this to note if a peer's lbard has restarted
  int offset=0;
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
  
  return offset;
}

