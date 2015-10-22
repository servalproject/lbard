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

#define STATUS_FILE "/tmp/lbard.status"

int status_dump()
{
  FILE *f=fopen(STATUS_FILE,"w");
  if (!f) return -1;

  fprintf(f,"<HTML>\n<HEAD>lbard version %s status dump @ T=%lldms</head><body>\n",
	  VERSION_STRING,gettime_ms());

  int i;
  fprintf(f,"<table border=1 padding=2 spacing=2><tr><th>BID Prefix</th><th>Last calculated priority</th></tr>\n");
  for (i=0;i<bundle_count;i++)
    fprintf(f,"<tr><td>%02x%02x%02x%02x%02x%02x*</td><td>0x%08llx (%lld)</td></tr>\n",
	    bundles[i].bid[0],bundles[i].bid[1],bundles[i].bid[2],
	    bundles[i].bid[3],bundles[i].bid[4],bundles[i].bid[5],
	    bundles[i].last_priority,bundles[i].last_priority);
  fprintf(f,"</table>\n");

  fprintf(f,"</body>\n");
  
  fclose(f);
  
  return 0;
}
