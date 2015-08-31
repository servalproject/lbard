/*
Serval Low-Bandwidth Rhizome Transport
Copyright (C) 2015 Serval Project Inc.

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

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <time.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <assert.h>

#include "lbard.h"

#include "golay.h"
#include "fec-3.0.1/fixed.h"
void encode_rs_8(data_t *data, data_t *parity,int pad);
int decode_rs_8(data_t *data, int *eras_pos, int no_eras, int pad);
#define FEC_LENGTH 32
#define FEC_MAX_BYTES 223

extern unsigned char my_sid[32];
extern char *my_sid_hex;
extern char *servald_server;
extern char *credential;
extern char *prefix;

int radio_read_bytes(int serialfd)
{
  unsigned char buf[8192];
  ssize_t count =
    read_nonblock(serialfd,buf,8192);

  if (count>0)
    radio_receive_bytes(buf,count);
  return count;
}

int radio_send_message(int serialfd, unsigned char *buffer,int length)
{
  unsigned char out[3+FEC_MAX_BYTES+FEC_LENGTH+3];
  int offset=0;

  // Encapsulate message in Reed-Solomon wrapper and send.
  unsigned char parity[FEC_LENGTH];

  // Calculate RS parity
  if (length>FEC_MAX_BYTES||length<0) {
    fprintf(stderr,"%s(): Asked to send packet of illegal length"
	    " (asked for %d, valid range is 0 -- %d)\n",
	    __FUNCTION__,length,FEC_MAX_BYTES);
    return -1;
  }
  encode_rs_8(buffer,parity,FEC_MAX_BYTES-length);

  // Golay encode length for safety
  int len=length;
  golay_encode((unsigned char *)&len);
  out[offset++]=(len>>0)>>0;
  out[offset++]=(len>>8)>>0;
  out[offset++]=(len>>16)>>0;
  
  // Then, the packet body
  bcopy(buffer,&out[offset],length);
  offset+=length;

  // Next comes the RS parity bytes
  bcopy(parity,&out[offset],FEC_LENGTH);
  offset+=FEC_LENGTH;

  // Finally, we encode the length again, so that we can more easily verify
  // that we have a complete and uninterrupted packet.  To avoid ambiguity, we
  // encode not the actual length, but a value that we know corresponds to the
  // length, but is not a valid start of packet length.
  // We have 12 bits to play with, and FEC_MAX_BYTES is only 223 bytes, so let's
  // multiply the length by 13 and add (FEC_MAX_BYTES+1).  This will give a result
  // in the range 224 - 3123 bytes.
  len=(length*13)+FEC_MAX_BYTES+1;
  golay_encode((unsigned char *)&len);
  out[offset++]=(len>>0)>>0;
  out[offset++]=(len>>8)>>0;
  out[offset++]=(len>>16)>>0;  

  assert( offset <= (3+FEC_MAX_BYTES+FEC_LENGTH+3) );

  write_all(serialfd,out,offset);
  
  return -1;
}

// This need only be the maximum packet size
#define RADIO_RXBUFFER_SIZE 1000
unsigned char radio_rx_buffer[RADIO_RXBUFFER_SIZE];

int radio_receive_bytes(unsigned char *bytes,int count)
{
  fprintf(stderr,"CHECKPOINT: %s:%d %s()\n",__FILE__,__LINE__,__FUNCTION__);
  
  for(int i=0;i<count;i++) {
    bcopy(&radio_rx_buffer[1],&radio_rx_buffer[0],RADIO_RXBUFFER_SIZE-1);
    radio_rx_buffer[RADIO_RXBUFFER_SIZE-1]=bytes[i];

    // Decode end of packet length field
    int golay_end_errors;
    int end_length=golay_decode(&golay_end_errors,&radio_rx_buffer[RADIO_RXBUFFER_SIZE-3])-(FEC_MAX_BYTES+1);

    // Ignore packet if it does not satisfy !((n-FEC_MAX_BYTES-1)%13)
    if (end_length%13) continue;
    // Get actual length of packet
    int length=end_length/13;
    // Work out where start length will be. This will be 3+length+FEC_LENGTH+3 bytes before the end
    int candidate_start_offset=
      RADIO_RXBUFFER_SIZE-(3+length+FEC_LENGTH+3);
    int golay_start_errors=0;
    int start_length=golay_decode(&golay_start_errors,
				  &radio_rx_buffer[candidate_start_offset]);

    
    // Ignore packet if the two length fields do not agree.
    if (start_length!=length) continue;

    // Now do RS check on packet contents
    unsigned char *body = &radio_rx_buffer[candidate_start_offset+3];
    int rs_error_count = decode_rs_8(body,NULL,0,FEC_MAX_BYTES-length);
    
    fprintf(stderr,"CHECKPOINT: %s:%d %s()\n",__FILE__,__LINE__,__FUNCTION__);
    saw_message(body,length,
		my_sid_hex,prefix,servald_server,credential);
    
  }

  return 0;
}
