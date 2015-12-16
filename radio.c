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
#include <string.h>
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

int serial_errors=0;

int radio_read_bytes(int serialfd,int monitor_mode)
{
  unsigned char buf[8192];
  ssize_t count =
    read_nonblock(serialfd,buf,8192);

  if (count>0)
    radio_receive_bytes(buf,count,monitor_mode);
  return count;
}

/* 
   On the MR3020 hardware, there is a 3-position slider switch.
   If the switch is in the centre position, then we enable high
   power output.

   WARNING: You will need a spectrum license to be able to operate
   in high-power mode, because the CSMA RFD900a firmware is normally
   only legal at an EIRP of 3dBm or less, which allowing for a +3dB
   antanna means that 1dBm is the highest TX power that is realistically
   safe.

   Because of the above, we require that /dos/hipower.en exist on the file
   system as well as the switch being in the correct position.  The Mesh
   Extender default image does not include the /dos/hipower.en file, and it
   must be added manually to enable hi-power mode.
 */
unsigned char hipower_en=0;
unsigned char hipower_switch_set=0;
long long hi_power_timeout=3000; // 3 seconds between checks
long long next_check_time=0;
int radio_set_tx_power(int serialfd)
{
  char *safety_file="/dos/hipower.en";
  char *gpio_file="/sys/kernel/debug/gpio";
  char *gpio_string=" gpio-18  (sw1                 ) in  lo";

  if (next_check_time<gettime_ms()) {
    hipower_en=0;
    hipower_switch_set=0;
    next_check_time=gettime_ms()+hi_power_timeout;

    FILE *f=fopen(safety_file,"r");
    if (f) {
      hipower_en=1;
      fclose(f);
    }
    f=fopen(gpio_file,"r");
    if (f) {
      char line[1024];
      line[0]=0; fgets(line,2014,f);
      while(line[0]) {
	if (!strncmp(gpio_string,line,strlen(gpio_string)))
	  hipower_switch_set=1;
	
	line[0]=0; fgets(line,2014,f);
      }
      fclose(f);
    }
  }

  if (hipower_switch_set&&hipower_en) {
    fprintf(stderr,"Setting radio to hipower -- you need a special spectrum license to do this!\n");
    if (write_all(serialfd,"!H",2)==-1) serial_errors++; else serial_errors=0;
  } else {
    if (0) fprintf(stderr,"Setting radio to lowpower mode (flags %d:%d) -- probably ok under Australian LIPD class license, but you should check.\n",
		   hipower_switch_set,hipower_en);
    if (write_all(serialfd,"!L",2)==-1) serial_errors++; else serial_errors=0;
  }

  return 0;
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
  unsigned char golay_len[3];

  golay_len[0]=(len&0xff)>>0;
  golay_len[1]=(len>>8)&0x0f;
  golay_len[2]=0;
  golay_encode(golay_len);
  out[offset++]=golay_len[0];
  out[offset++]=golay_len[1];
  out[offset++]=golay_len[2];
  
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
  golay_len[0]=(len&0xff)>>0;
  golay_len[1]=(len>>8)&0x0f;
  golay_len[2]=0;
  golay_encode(golay_len);
  out[offset++]=golay_len[0];
  out[offset++]=golay_len[1];
  out[offset++]=golay_len[2];

  assert( offset <= (3+FEC_MAX_BYTES+FEC_LENGTH+3) );

  // Now escape any ! characters, and append !! to the end for the RFD900 CSMA
  // packetised firmware.

  unsigned char escaped[2+offset*2+2];
  int elen=0;
  int i;

  // Begin with clear TX buffer command
  escaped[elen++]='!'; escaped[elen++]='C';

  // Then stuff the escaped bytes to send
  for(i=0;i<offset;i++) {
    if (out[i]=='!') {
      escaped[elen++]='!'; escaped[elen++]='.';
    } else escaped[elen++]=out[i];
  }
  // Finally include TX packet command
  escaped[elen++]='!'; escaped[elen++]='!';
  
  radio_set_tx_power(serialfd);
  
  if (write_all(serialfd,escaped,elen)==-1) {
    serial_errors++;
    return -1;
  } else {
    serial_errors=0;
    return 0;
  }
}

// This need only be the maximum packet size
#define RADIO_RXBUFFER_SIZE 1000
unsigned char radio_rx_buffer[RADIO_RXBUFFER_SIZE];

int radio_receive_bytes(unsigned char *bytes,int count,int monitor_mode)
{
  int i,j;

  if (debug_pieces) {
    fprintf(stderr,"Read %d bytes from radio:\n",count);
    for(i=0;i<count;i+=32) {
      for(j=0;j<32;j++) {
	if (i+j<count) fprintf(stderr," %02x",bytes[i+j]); else break;
      }
      for(;j<32;j++) fprintf(stderr,"   ");
      fprintf(stderr,"  ");
      for(j=0;j<32;j++) {
	if (i+j<count) {
	  if (bytes[i+j]>=' '&&bytes[i+j]<0x7e)
	    fprintf(stderr,"%c",bytes[i+j]);
	  else fprintf(stderr,".");
	}
      }
      fprintf(stderr,"\n");
    }
  }

  for(i=0;i<count;i++) {

    bcopy(&radio_rx_buffer[1],&radio_rx_buffer[0],RADIO_RXBUFFER_SIZE-1);
    radio_rx_buffer[RADIO_RXBUFFER_SIZE-1]=bytes[i];

    if ((radio_rx_buffer[RADIO_RXBUFFER_SIZE-1]==0xdd)
	&&(radio_rx_buffer[RADIO_RXBUFFER_SIZE-8]==0xec)
	&&(radio_rx_buffer[RADIO_RXBUFFER_SIZE-9]==0xce))
      {
	fprintf(stderr,"GPIO ADC values = ");
	for(int j=0;j<6;j++) {
	  fprintf(stderr,"%s0x%02x",
		  j?",":"",
		  radio_rx_buffer[RADIO_RXBUFFER_SIZE-7+j]);
	}
	fprintf(stderr,"\n");
      } else if ((radio_rx_buffer[RADIO_RXBUFFER_SIZE-1]==0x55)
	&&(radio_rx_buffer[RADIO_RXBUFFER_SIZE-8]==0x55)
	&&(radio_rx_buffer[RADIO_RXBUFFER_SIZE-9]==0xaa))
      {
	// Found RFD900 CSMA envelope
	int last_rx_rssi=radio_rx_buffer[RADIO_RXBUFFER_SIZE-7];
	// int average_remote_rssi=radio_rx_buffer[RADIO_RXBUFFER_SIZE-6];
	int radio_temperature=radio_rx_buffer[RADIO_RXBUFFER_SIZE-5];
	int packet_len=radio_rx_buffer[RADIO_RXBUFFER_SIZE-4];
	int buffer_space=radio_rx_buffer[RADIO_RXBUFFER_SIZE-3];
	buffer_space+=radio_rx_buffer[RADIO_RXBUFFER_SIZE-2]*256;

	message_buffer_length+=
	  snprintf(&message_buffer[message_buffer_length],
		   message_buffer_size-message_buffer_length,
		   "Saw RFD900 CSMA Data frame: temp=%dC, last rx RSSI=%d, frame len=%d\n",
		   radio_temperature, last_rx_rssi,
		   packet_len);

#define TRAILER_LEN 9
	// Decode end of packet length field
	int golay_end_errors;
	int end_length=golay_decode(&golay_end_errors,&radio_rx_buffer[RADIO_RXBUFFER_SIZE-3-TRAILER_LEN])-(FEC_MAX_BYTES+1);
	// Get actual length of packet
	int length=end_length/13;
	// Work out where start length will be. This will be 3+length+FEC_LENGTH+3 bytes before the end
	// But take another 9 bytes for the radio frame trailer
	int candidate_start_offset=
	  RADIO_RXBUFFER_SIZE-(3+length+FEC_LENGTH+3)-TRAILER_LEN;
	int golay_start_errors=0;
	int start_length=golay_decode(&golay_start_errors,
				      &radio_rx_buffer[candidate_start_offset]);

	// Ignore packet if it does not satisfy !((n-FEC_MAX_BYTES-1)%13)
	if (end_length%13) {
	  if (message_buffer_length) message_buffer_length--; // chop NL
	  message_buffer_length+=
	    snprintf(&message_buffer[message_buffer_length],
		     message_buffer_size-message_buffer_length,
		     ", LENGTH MODULO CHECK FAIL (end_length=%d, mod 13 = %d, start_length=%d)\n",
		     end_length,end_length%13,start_length);
	  
	  continue;
	}
	
	// fprintf(stderr,"  This isn't a packet of %d bytes (start_length=%d)\n",
	// length,start_length);
	
	// Ignore packet if the two length fields do not agree.
	if (start_length!=length) {
	  if (message_buffer_length) message_buffer_length--; // chop NL
	  message_buffer_length+=
	    snprintf(&message_buffer[message_buffer_length],
		     message_buffer_size-message_buffer_length,
		     ", LENGTH CHECK FAIL (start_length=%d, length=%d)\n",
		     start_length,length);
	  continue;
	}
	
	// Now do RS check on packet contents
	unsigned char *body = &radio_rx_buffer[candidate_start_offset+3];
	int rs_error_count = decode_rs_8(body,NULL,0,FEC_MAX_BYTES-length);
	
	if (rs_error_count>=0&&rs_error_count<8) {
	  if (0) fprintf(stderr,"CHECKPOINT: %s:%d %s() error counts = %d,%d,%d for packet of %d bytes.\n",
			 __FILE__,__LINE__,__FUNCTION__,
			 golay_end_errors,rs_error_count,golay_start_errors,length);
	  
	  saw_message(body,length,
		      my_sid_hex,prefix,servald_server,credential);
	  
	  // attach presumed SID prefix
	  if (message_buffer_length) message_buffer_length--; // chop NL
	  message_buffer_length+=
	    snprintf(&message_buffer[message_buffer_length],
		     message_buffer_size-message_buffer_length,
		     ", FEC OK : sender SID=%02x%02x%02x%02x%02x%02x*\n",
		     body[0],body[1],body[2],body[3],body[4],body[5]);

	  {
	    char sender_prefix[128];
	    char monitor_log_buf[1024];
	    bytes_to_prefix(&body[0],sender_prefix);
	    snprintf(monitor_log_buf,sizeof(monitor_log_buf),
		     "Saw RFD900 CSMA Data frame: temp=%dC, last rx RSSI=%d,"
		     " frame len=%d, FEC OK",
		     radio_temperature, last_rx_rssi,
		     packet_len);
	    
	    monitor_log(sender_prefix,NULL,monitor_log_buf);
	  }
	} else {
	  if (message_buffer_length) message_buffer_length--; // chop NL
	  message_buffer_length+=
	    snprintf(&message_buffer[message_buffer_length],
		     message_buffer_size-message_buffer_length,
		     ", FEC FAIL (rs_error_count=%d)\n",
		     rs_error_count);
	}
	
      }
  }
  return 0;
}
