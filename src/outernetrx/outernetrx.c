/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2018 Paul Gardner-Stephen

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
#include <sys/socket.h>
#include <sys/un.h>

#include "sync.h"
#include "lbard.h"
#include "serial.h"
#include "version.h"
#include "radios.h"
#include "code_instrumentation.h"

int outernet_socket=-1;

int outernet_rx_serviceloop(void)
{
  unsigned char buffer[4096];
  ssize_t bytes_recv;
  int retVal=0;
  
  LOG_ENTRY;

  do {
    bytes_recv = recvfrom( outernet_socket, buffer, sizeof(buffer), 0, 0, 0 );
    while(bytes_recv>0) {
      LOG_NOTE("Received %d bytes via Outernet UNIX domain socket",bytes_recv);
      dump_bytes(stderr,"Packet",buffer,bytes_recv);
      
      bytes_recv = recvfrom( outernet_socket, buffer, sizeof(buffer), 0, 0, 0 );
    }
  } while(0);

  LOG_EXIT;
  return retVal;
}

int outernet_rx_setup(char *socket_filename)
{
  int exitVal=0;

  LOG_ENTRY;
  do {
  
    // Open socket for reading from an outernet receiver.
    // (note that we can theoretically do this while also talking
    // to a radio, because the outernet one-way protocol is quite
    // separate from the usual LBARD protocol.
    LOG_NOTE("Trying to open Outernet socket");
    
    outernet_socket = socket( AF_UNIX, SOCK_DGRAM, 0 );
    
    if( outernet_socket < 0 ) {
      LOG_ERROR("socket() failed: (%i) %m", errno );
      exitVal=-1;
      break;
    }
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    if (setsockopt(outernet_socket, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
      LOG_ERROR("setsockopt failed: (%i) %m", errno );
      exitVal=-1;
      break;
    }
    
    // Bind to unix socket
    struct sockaddr_un sun;
    sun.sun_family = AF_UNIX;
    snprintf( sun.sun_path, sizeof( sun.sun_path ), "%s", socket_filename );

    if( -1 == connect( outernet_socket, (struct sockaddr *)&sun, sizeof( struct sockaddr_un ))) {
      LOG_ERROR("connect failed: (%i) %m", errno );
      exitVal=-1; break;
    }

    LOG_NOTE("Opened unix socket '%s' for outernet rx",socket_filename);
  } while (0);

  LOG_EXIT;
  return exitVal;  
}
