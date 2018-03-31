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

int message_parser_54(struct peer_state *sender,unsigned char *prefix,
		      char *servald_server, char *credential,
		      unsigned char *msg,int length)
{
  int offset=0;
  {
    fprintf(stderr,"Saw timestamp message.\n");
    offset++;
    int stratum=msg[offset++];
    struct timeval tv;
    bzero(&tv,sizeof (struct timeval));
    for(int i=0;i<8;i++) tv.tv_sec|=msg[offset++]<<(i*8);
    for(int i=0;i<3;i++) tv.tv_usec|=msg[offset++]<<(i*8);
    /* XXX - We don't do any clever NTP-style time correction here.
       The result will be only approximate, probably accurate to only
       ~10ms - 100ms per stratum, and always running earlier and earlier
       with each stratum, as we fail to correct the received time for 
       transmission duration.
       We can at least try to fix this a little:
       1. UHF radio serial speed = 230400bps = 23040cps.
       2. Packets are typically ~250 bytes long.
       3. Serial TX speed to radio is thus ~10.8ms
       4. UHF Radio air speed is 128000bps.
       5. Radio TX time is thus 250*8/128000= ~15.6ms
       6. Total minimum delay is thus ~26.4ms
       
       Thus we will make this simple correction of adding 26.4ms.
       
       The next challenge is if we have multiple sources with the same stratum
       giving us the time.  In that case, we need a way to choose a winner, since
       we are not implementing fancy NTP-style time integration algorithms. The
       trick is to get something simple, that stops clocks jumping backwards and
       forwards allover the shop.  A really simple approach is to have a timeout
       when updating the time, and ignore updates from the same time stratum for
       the next several minutes.  We should also decay our stratum if we have not
       heard from an up-stream clock lately, so that we always converge on the
       freshest clock.  In fact, we can use the slow decay to implement this
       quasi-stability that we seek.
    */	
    tv.tv_usec+=26400;
    
    char sender_prefix[128];	
    sprintf(sender_prefix,"%s*",p->sid_prefix);
    
    saw_timestamp(sender_prefix,stratum,&tv);
    
    // Also record time delta between us and this peer in the relevant peer structure.
    // The purpose is to that the bundle/activity log can be more easily reconciled with that
    // of other mesh extenders.  By being able to relate the claimed time of each mesh extender
    // against each other, we can hopefully quite accurately piece together the timing of bundle
    // transfers via UHF, for example.
    time_t now =time(0);
    long long delta=(long long)now-(long long)p->last_timestamp_received;
    fprintf(stderr,"Logging timestamp message from %s (delta=%lld).\n",sender_prefix,delta);
    if (delta<0) {
      fprintf(stderr,"Correcting last timestamp report time to be in the past, not future.\n");
      p->last_timestamp_received=0;
    }
    if (delta>60) {
      fprintf(stderr,"Logging timestamp message, since >60 seconds since last seen from this peer.\n");	  
      p->last_timestamp_received=now;
      FILE *bundlelogfile=NULL;
      if (debug_bundlelog) {
	bundlelogfile=fopen(bundlelog_filename,"a");
	if (bundlelogfile) {
	  fprintf(bundlelogfile,"%lld:T+%lldms:PEERTIME:%s:%lld:%s",
		  (long long)now,
		  (long long)(gettime_ms()-start_time),sender_prefix,
		  (long long)(tv.tv_sec-now),ctime(&now));
	  fprintf(stderr,"Logged timestamp message.\n");
	  fclose(bundlelogfile);
	} else perror("Could not open bundle log file");
      } else fprintf(stderr,"Logging timestamps disabled via debug_bundlelog.\n");
    } else fprintf(stderr,"Not logging timestamp message, since we logged one just recently (%lld-%lld = %lld).\n",
		   (long long)now,(long long)p->last_timestamp_received,
		   (long long)now-(long long)p->last_timestamp_received);		       
    
  }

  return offset;
}

