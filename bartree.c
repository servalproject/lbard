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

bundle_node *bartree=NULL;

/*
  Noting a BAR now means trying to insert it into the tree.
 */
int peer_note_bar(struct peer_record *p,
		  long long bid_prefix_bin,
		  long long version,
		  long long recipient_sid_prefix_bin,
		  int size_byte)
{
  int bit_number=63;
  bundle_node **n=&bartree;
  while (*n) {
    // Stop searching if we have found the right node.
    if (bid_prefix_bin==(*n)->bar.bid_prefix_bin) break;

    // Else drill down to next level
    int bit=(bid_prefix_bin>>bit_number)&1;
    if (bit) n=&(*n)->right; else n=&(*n)->left;

    // Move along BID prefix to identify where it goes
    bit--;
    if (bit<0) return -1;
  }
  if (!(*n)) {
    // Allocate new node if it doesn't already exist
    *n=calloc(sizeof(bundle_node),1);
    // If we have had to allocate the node, then we need to push the leaf node down
    // a layer as well.
  } else {
    //
  }
}
