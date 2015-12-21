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
#include <sys/time.h>

#include "lbard.h"

long long next_time_update_allowed_after=0;

int saw_timestamp(char *sender_prefix,int stratum, struct timeval *tv)
{
  printf("Saw timestamp from %s: stratum=0x%02x, our stratum=0x%02x.0x%02x\n",
	 sender_prefix,stratum,my_time_stratum>>8,my_time_stratum&0xff);
  
  if (tv->tv_usec>999999) { tv->tv_sec++; tv->tv_usec-=1000000; }

  if (gettime_ms()>next_time_update_allowed_after) {
    if ((stratum<(my_time_stratum>>8))) {
      // Found a lower-stratum time than our own, and we have enabled time
      // slave mode, so set system time.
      // Then update our internal timers accordingly
      if (time_slave&&(!monitor_mode)) {
	struct timeval before,after;
	gettimeofday(&before,NULL);
	settimeofday(tv,NULL);
	gettimeofday(&after,NULL);
	long long delta=
	  after.tv_sec*1000+(after.tv_usec/1000)
	  -
	  before.tv_sec*1000+(before.tv_usec/1000);
	last_message_update_time+=delta;
	congestion_update_time+=delta;

	// Don't touch time again for a little while
	// (Updating time possibly several times per second might upset things)
	next_time_update_allowed_after=gettime_ms()+20000;
      }
      // By adding only one milli-strata, we effectively match the stratum that
      // updated us for the next 256 UHF packet transmissions. This should give
      // us the stability we desire.
      printf("Saw timestamp from %s with stratum 0x%02x,"
	     " which is better than our stratum of 0x%02x.0x%02x\n",
	     sender_prefix,stratum,my_time_stratum>>8,my_time_stratum&0xff);
      my_time_stratum=(stratum<<8)+1;
    }
  }
  if (monitor_mode)
    {
      char monitor_log_buf[1024];
      time_t current_time=tv->tv_sec;
      struct tm tm;
      localtime_r(&current_time,&tm);
      
      snprintf(monitor_log_buf,sizeof(monitor_log_buf),
	       "Timestamp: stratum=0x%02X, "
	       "time %04d/%02d/%02d %02d:%02d.%02d (%14lld.%06lld)",
	       stratum,
	       tm.tm_year+1900,tm.tm_mon,tm.tm_mday+1,
	       tm.tm_hour,tm.tm_min,tm.tm_sec,
	       (long long)tv->tv_sec,(long long)tv->tv_usec);
      
      monitor_log(sender_prefix,NULL,monitor_log_buf);
    }
  return 0;
}
