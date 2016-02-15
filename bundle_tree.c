/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2016 Serval Project Inc.

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

int bundle_calculate_tree_key(uint8_t bundle_tree_key[SYNC_KEY_LEN],
			      uint8_t sync_tree_salt[SYNC_SALT_LEN],
			      char *bid,
			      long long version,
			      long long length,
			      char *filehash)
{
  /*
    Calculate a sync key for this bundle.
    Sync keys are relatively short, only 64 bits, as this is still sufficient to
    maintain a very low probability of colissions, provided that each peer has less
    than 2^32 bundles.

    Ideally we would hash the entire manifest of a bundle, but that would require us
    to retrieve each manifest, and we would rather not require that.  So instead we
    will use the BID, length, version and filehash as inputs.  This combination means
    that in the unlikely event of a colission, updating the bundle is almost certain
    to resolve the colission.  Thus the natural human response of sending another
    message if the first doesn't get through is likely to resolve the problem.

    The relatively small key space does however leave us potentially vulnerable to a
    determined adversary to find coliding hashes to disrupt communications. We will
    need to do something about this in due course. This could probably be protected
    by using a random salt between pairs of peers when they do the sync process, so
    that colissions cannot be arranged ahead of time.

    Using a salt, and changing it periodically, would also provide implicit protection
    against colissions of any source, so it is probably a really good idea to
    implement.  The main disadvantage is that we need to calculate all the hashes for
    all the bundles we hold whenever we talk to a new peer.  We could, however,
    use a salt for all peers which we update periodically, to offer a good compromise
    between computational cost and protection against accidental and intentional
    colissions.
    
    Under this strategy, we need to periodically recalculate the sync key, i.e, hash,
    for each bundle we hold, and invalidate the sync tree for each peer when we do so.

    ... HOWEVER, we need both sides of a conversation to have the same salt, which
    wouldn't work under that scheme.

    So for now we will employ a salt, but it will probably be fixed until we decide on
    a good solution to this set of problems.
  */

  char lengthstring[80];
  snprintf(lengthstring,80,"%llx:%llx",length,version);
  
  struct sha1nfo sha1;  
  sha1_init(&sha1);
  sha1_write(&sha1,(const char *)sync_tree_salt,SYNC_SALT_LEN);
  sha1_write(&sha1,bid,strlen(bid));
  sha1_write(&sha1,filehash,strlen(filehash));
  sha1_write(&sha1,lengthstring,strlen(lengthstring));
  bcopy(sha1_result(&sha1),bundle_tree_key,SYNC_KEY_LEN);
  return 0;  
}

int sync_update_peer_sequence_acknowledgement_field(int peer,uint8_t *msg)
{
  int len=5;
  // Acknowledge what we have seen from the remote side
  msg[len++]=peer_records[peer]->last_remote_sequence_acknowledged;
  msg[len++]=peer_records[peer]->remote_sequence_bitmap&0xff;
  msg[len++]=(peer_records[peer]->remote_sequence_bitmap>>8)&0xff;
  return 0;
}

int sync_peer_window_has_space(int peer)
{
  int space=peer_records[peer]->last_local_sequence_number
    -peer_records[peer]->last_local_sequence_number_acknowledged;
  if (space<0) space+=256;
  if (space>0) return 1; else return 0;
}

int sync_by_tree_stuff_packet(int *offset,int mtu, unsigned char *msg_out)
{
  int peer=random_active_peer();
  if (peer_records[peer]->retransmit_requested) {
    // Retransmit last transmission.
    int slot=peer_records[peer]->retransmition_sequence&15;
    // Update acknowledgement bytes to reflect current situation.
    sync_update_peer_sequence_acknowledgement_field
      (peer,peer_records[peer]->retransmit_buffer[slot]);
    if (!append_bytes(offset,mtu,msg_out,
		      peer_records[peer]->retransmit_buffer[slot],
		      peer_records[peer]->retransmit_lengths[slot]))
      peer_records[peer]->retransmit_requested=0;
  }
  int space=mtu-(*offset);
  if (sync_peer_window_has_space(peer)&&(space>10)) {
    /* Try sending something new.
       XXX - Check send-queue for this peer to see if we should
       be sending part of a bundle. */
    uint8_t msg[256];
    int len=0;
    msg[len++]='S'; // Sync message
    // SID prefix of recipient
    msg[len++]=peer_records[peer]->sid_prefix_bin[0];
    msg[len++]=peer_records[peer]->sid_prefix_bin[1];
    msg[len++]=peer_records[peer]->sid_prefix_bin[2];
    // Sequence number (our side)
    peer_records[peer]->last_local_sequence_number++;
    peer_records[peer]->last_local_sequence_number&=0xff;
    msg[len++]=peer_records[peer]->last_local_sequence_number;
    // Acknowledge what we have seen from the remote side
    msg[len++]=peer_records[peer]->last_remote_sequence_acknowledged;
    msg[len++]=peer_records[peer]->remote_sequence_bitmap&0xff;
    msg[len++]=(peer_records[peer]->remote_sequence_bitmap>>8)&0xff;
    int used=sync_build_message(&peer_records[peer]->sync_state,
				&msg[len],space-4);
    len+=used;
    append_bytes(offset,mtu,msg_out,msg,len);
  }
  return 0;
}

int sync_tree_prepare_tree(int peer)
{
  // Default fixed salt.
  uint8_t sync_tree_salt[SYNC_SALT_LEN]={0xa9,0x1b,0x8d,0x11,0xdd,0xee,0x20,0xd0};

  // Clear tree
  bzero(&peer_records[peer]->sync_state,sizeof(peer_records[peer]->sync_state));
  
  for(int i=0;i<bundle_count;i++) {
    sync_key_t key;
    bundle_calculate_tree_key(key.key,
			      sync_tree_salt,
			      bundles[i].bid,
			      bundles[i].version,
			      bundles[i].length,
			      bundles[i].filehash);
    sync_add_key(&peer_records[peer]->sync_state,&key);
  }
  return 0;
}

XXX - Setup tree when a new peer is detected.

XXX - Request sync data to send when stuffing packets (see txmessages.c:474).

XXX - Implement quasi-reliable receipt of packets from peers (out of order delivery is fine, provided that we get the omitted frames eventually. lost frames affect efficiency, not completeness of procedure)

XXX - Implement sending of bundle pieces based on sync data discoveries.

XXX - Implement telling sender when they send data that we already have.
