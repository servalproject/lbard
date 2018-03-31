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

int message_parser_53(struct peer_state *sender,unsigned char *prefix,
		      char *servald_server, char *credential,
		      unsigned char *msg,int length)
{
  int offset=0;
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
  
  return offset;
}

