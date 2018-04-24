/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2015-2018 Serval Project Inc.

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
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>

#include "sync.h"
#include "lbard.h"
#include "serial.h"
#include "version.h"
#include "radios.h"

int log_rssi(struct peer_state *p,int rssi)
{
  // Shuffle old entries down
  int i;
  for(i=RSSI_LOG_SIZE-1;i>0;i--)
    {
      p->recent_rssis[i]=p->recent_rssis[i-1];
      p->recent_rssi_times[i]=p->recent_rssi_times[i-1];
    }
  if (p->rssi_log_count<RSSI_LOG_SIZE)
    p->rssi_log_count++;

  // Put new entry at top
  p->recent_rssis[0]=rssi;
  p->recent_rssi_times[0]=gettime_ms();
  return 0;
}

int log_rssi_timewarp(long long delta)
{
  for(int p=0;p<peer_count;p++)
    {
      if (!peer_records[p]) continue;
      for(int i=0;i<peer_records[p]->rssi_log_count;i++) {
	peer_records[p]->recent_rssi_times[i]+=delta;
      }
    }
  return 0;
}
