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


/*
  See also drv_outernet.c, and outernet_uplink_build_packet()
  in particular.

  The packets we send are protected by RAID-5 like parity,
  which can be used to reconstruct a single missing packet
  from each parity zone.  The initial implementation uses 
  a parity zone of four packets, with 1/4 of the data being
  parity, that is the parity expands the dataa to 4/3 the
  original size, and thus allows one in four packets to be
  lost, without loss of data.

  There are five lanes of transfer simultaneously, so we
  need to keep track of those.

  The packet format is:

  lane number - one byte
  sequence number + stop and start markers - two bytes
  logical MTU - one byte
  data - variable length
  parity stripe - variable length

  More specifically, the logical MTU specifies the total packet
  size being used, for the purposes of determining the data and
  parity strip sizes in each packet. 

*/

struct outernet_rx_bundle {
  int offset;
  int parity_zone_number;
  unsigned char *data;
#define MAX_DATA_BYTES 256
  unsigned char parity_zone[MAX_DATA_BYTES*4];
  char parity_zone_bitmap;

  char waitingForStart;
};

#define MAX_LANES 5
struct outernet_rx_bundle outernet_rx_bundles[MAX_LANES];


int outernet_rx_saw_packet(unsigned char *buffer,int bytes)
{
  int retVal=0;
  LOG_ENTRY;

  do {

    unsigned int lane=buffer[0];
    unsigned int packet_mtu=buffer[3];

    if (lane>=MAX_LANES) {
      LOG_ERROR("Outernet packet is for lane #%d (we only support 0 -- %d)",
		lane,MAX_LANES-1);
      retVal=-1;
      break;
    }
    
    // 3/4 of the usable bytes are available for data
    int data_bytes=(packet_mtu-3)*3/4;
    int parity_bytes=data_bytes/3;
    if (parity_bytes*3!=data_bytes) {
      LOG_ERROR("Parity stripe size problem. MTU=%d, data_bytes=%d, parity_bytes=%d",
		packet_mtu,data_bytes,parity_bytes);
      retVal=-1;
      break;
    }

    int start_flag=0;
    int end_flag=0;
    int sequence_number=buffer[1]+(buffer[2]<<8);
    sequence_number &= 0x3fff;
    if (buffer[2]&0x40) start_flag=1;
    if (buffer[2]&0x80) end_flag=1;

    int parity_zone_size=data_bytes*4;
    int parity_zone_offset=(sequence_number*data_bytes)%parity_zone_size;
    int parity_zone_start=(sequence_number*data_bytes)-parity_zone_offset;
    int parity_zone_number=sequence_number/4;
    int parity_zone_slice=sequence_number&3;
    
    unsigned char *data=&buffer[4];
    unsigned char *parity=&buffer[4+data_bytes];

    LOG_NOTE("Received bundle piece in lane #%d, sequence #%d (start=%d, end=%d) (parity zone #%d, packet %d)",
	     lane,
	     sequence_number,start_flag,end_flag,
	     parity_zone_number,parity_zone_slice);
    dump_bytes(stderr,"Bundle bytes",data,data_bytes);
    dump_bytes(stderr,"Parity bytes",parity,parity_bytes);
    
  } while(0);
  
  LOG_EXIT;
  return retVal;
}
  

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

      if (outernet_rx_saw_packet(buffer,bytes_recv)) {
	retVal=-1;
	LOG_ERROR("outernet_rx_saw_packet() reported an error");
      }
      
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

    // Initialise data RX structures
    for(int i=0;i<MAX_LANES;i++) {
      outernet_rx_bundles[i].waitingForStart=1;
      outernet_rx_bundles[i].data=NULL;
    }
    
    outernet_socket = socket( AF_UNIX, SOCK_DGRAM, 0 );
    
    if( outernet_socket < 0 ) {
      LOG_ERROR("socket() failed: (%i) %m", errno );
      exitVal=-1;
      break;
    }

    // UNIX domain datagram sockets have to be bound to their own end point
    // as well.  This is a little odd, but it is how they work.
    unlink(socket_filename);
    struct sockaddr_un client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sun_family = AF_UNIX;
    strncpy(client_addr.sun_path, socket_filename, 104);

    if (bind(outernet_socket, (struct sockaddr *) &client_addr, sizeof(client_addr)))
      {
	perror("bind()");
	LOG_ERROR("bind()ing UNIX socket client end point failed");
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
    
    LOG_NOTE("Opened unix socket '%s' for outernet rx",socket_filename);
  } while (0);

  LOG_EXIT;
  return exitVal;  
}
