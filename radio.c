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

#include "sync.h"
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
extern char *onepeer;

int serial_errors=0;


// Count radio transmissions seen, so that we can dynamically adjust the packet
// rate based on an estimate of channel congestion.
int radio_transmissions_seen=0;
int radio_transmissions_byus=0;

int radio_mode=RADIO_RFD900;
int radio_features=0;

int radio_get_type()
{
  return radio_mode;
}

int radio_set_type(int radio_type)
{
  radio_mode=radio_type;
  radio_features=0;
  
  return 0;
}

int radio_set_feature(int bitmask)
{
  radio_features|=bitmask;
  if (radio_features&RADIO_ALE_2G)
    fprintf(stderr,"Radio supports ALE 2G messaging (90 6-bit characters per message)\n");
  if (radio_features&RADIO_ALE_3G)
    fprintf(stderr,"Radio supports ALE 3G messaging (256 8-bit characters per message)\n");
  return 0;
}

int radio_read_bytes(int serialfd,int monitor_mode)
{
  unsigned char buf[8192];
  ssize_t count =
    read_nonblock(serialfd,buf,8192);

  errno=0;
  
  if (count>0)
    radio_receive_bytes(buf,count,monitor_mode);
  else
    {
      if (0&&debug_radio_rx) {
	printf("Failed to read bytes from radio: count=%d, errno=%d\n",
		(int)count,errno);
	perror("no radio bytes");
      }
    }
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
  char *safety_file="/dos/lowpower";
#if 0
  char *gpio_file="/sys/kernel/debug/gpio";
  char *gpio_string=" gpio-18  (sw1                 ) in  lo";
#endif

  if (next_check_time<gettime_ms()) {
    hipower_en=1;
    hipower_switch_set=0;
    next_check_time=gettime_ms()+hi_power_timeout;

    FILE *f=fopen(safety_file,"r");
    if (f) {
      hipower_en=0;
      fclose(f);
    }
#if 1
    hipower_switch_set=1;
#else
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
#endif
  }

  // XXX - We work around a bug in our SiK firmware where !H and !C don't clear !-mode
  // So we issue a command to clear the buffer after
  if (hipower_switch_set&&hipower_en) {
    printf("Setting radio to hipower\n");
    if (write_all(serialfd,"!HC",3)==-1) serial_errors++; else serial_errors=0;
  } else {
    printf("Setting radio to lowpower mode (flags %d:%d) -- probably ok under Australian LIPD class license, but you should check.\n",
		   hipower_switch_set,hipower_en);
    if (write_all(serialfd,"!LC",3)==-1) serial_errors++; else serial_errors=0;
  }

  return 0;
}

int dump_bytes(char *msg,unsigned char *bytes,int length)
{
  printf("%s:\n",msg);
  for(int i=0;i<length;i+=16) {
    printf("%04X: ",i);
    for(int j=0;j<16;j++) if (i+j<length) printf(" %02X",bytes[i+j]);
    printf("  ");
    for(int j=0;j<16;j++) {
      int c;
      if (i+j<length) c=bytes[i+j]; else c=' ';
      if (c<' ') c='.';
      if (c>0x7d) c='.';
      printf("%c",c);
    }
    printf("\n");
  }
  return 0;
}


int radio_send_message_rfd900(int serialfd,unsigned char *out, int offset)
{
  // Now escape any ! characters, and append !! to the end for the RFD900 CSMA
  // packetised firmware.

  unsigned char escaped[2+offset*2+2];
  int elen=0;
  int i;

  radio_set_tx_power(serialfd);

  // Sometimes the ! gets eaten here. Solution is to
  // send a non-! character first, so that even if !-mode
  // is set, all works properly.  this will also stop us
  // accidentally doing !!, which will send a packet.
  write(serialfd,"C!C",3);

  // Then stuff the escaped bytes to send
  for(i=0;i<offset;i++) {
    if (out[i]=='!') {
      escaped[elen++]='!'; escaped[elen++]='.';
    } else escaped[elen++]=out[i];
  }
  // Finally include TX packet command
  escaped[elen++]='!'; escaped[elen++]='!';
  
  if (debug_radio_tx) {
    dump_bytes("sending packet",escaped,elen);    
  }  
  
  if (write_all(serialfd,escaped,elen)==-1) {
    serial_errors++;
    return -1;
  } else {
    serial_errors=0;
    return 0;
  }
}

int radio_send_message(int serialfd, unsigned char *buffer,int length)
{
  unsigned char out[3+FEC_MAX_BYTES+FEC_LENGTH+3];
  int offset=0;

  // Encapsulate message in Reed-Solomon wrapper and send.
  unsigned char parity[FEC_LENGTH];

  // Calculate RS parity
  if (length>FEC_MAX_BYTES||length<0) {
    printf("%s(): Asked to send packet of illegal length"
	    " (asked for %d, valid range is 0 -- %d)\n",
	    __FUNCTION__,length,FEC_MAX_BYTES);
    return -1;
  }
  encode_rs_8(buffer,parity,FEC_MAX_BYTES-length);
  
  // Then, the packet body
  bcopy(buffer,&out[offset],length);
  offset+=length;

  // Next comes the RS parity bytes
  bcopy(parity,&out[offset],FEC_LENGTH);
  offset+=FEC_LENGTH;

  if (debug_radio_tx) {
    dump_bytes("sending packet",buffer,offset);
  }

  
  assert( offset <= (FEC_MAX_BYTES+FEC_LENGTH) );

  switch(radio_mode) {
  case RADIO_RFD900: radio_send_message_rfd900(serialfd,out,offset); break;
  case RADIO_BARRETT_HF: radio_send_message_barretthf(serialfd,out,offset); break;
  case RADIO_CODAN_HF: radio_send_message_codanhf(serialfd,out,offset); break;
  }

  // Don't forget to count our own transmissions
  radio_transmissions_byus++;

  return 0;
}

int radio_receive_bytes(unsigned char *bytes,int count,int monitor_mode)
{
  int i,j;

  if (debug_radio_rx) {
    printf("Read %d bytes from radio:\n",count);
    for(i=0;i<count;i+=32) {
      for(j=0;j<32;j++) {
	if (i+j<count) printf(" %02x",bytes[i+j]); else break;
      }
      for(;j<32;j++) printf("   ");
      printf("  ");
      for(j=0;j<32;j++) {
	if (i+j<count) {
	  if (bytes[i+j]>=' '&&bytes[i+j]<0x7e)
	    printf("%c",bytes[i+j]);
	  else printf(".");
	}
      }
      printf("\n");
    }
  }

  if (radio_get_type()==RADIO_RFD900) uhf_receive_bytes(bytes,count);
  else if (radio_get_type()==RADIO_CODAN_HF) hf_codan_receive_bytes(bytes,count);
  else if (radio_get_type()==RADIO_BARRETT_HF) hf_barrett_receive_bytes(bytes,count);

  return 0;
}

int saw_packet(unsigned char *packet_data,int packet_bytes,
	       char *my_sid_hex,char *prefix,
	       char *servald_server,char *credential)
{
  if (debug_radio) dump_bytes("packet before decode_rs",packet_data,packet_bytes);
  
  int rs_error_count = decode_rs_8(packet_data,NULL,0,
				   FEC_MAX_BYTES-packet_bytes+FEC_LENGTH);
  
  if (debug_radio) dump_bytes("received packet",packet_data,packet_bytes);

  char sender_prefix[128];
  bytes_to_prefix(&packet_data[0],sender_prefix);

  if (onepeer&&strncasecmp(sender_prefix,onepeer,strlen(sender_prefix))) {
    printf("Ignoring packet from SID %s* due to onepeer=%s\n",
	   sender_prefix,onepeer);
    return -1;
  }
  
  if (rs_error_count>=0&&rs_error_count<8) {
    if (0) printf("CHECKPOINT: %s:%d %s() error counts = %d for packet of %d bytes.\n",
		  __FILE__,__LINE__,__FUNCTION__,
		  rs_error_count,packet_bytes);
    
    saw_message(packet_data,packet_bytes-FEC_LENGTH,
		my_sid_hex,prefix,servald_server,credential);
    
    // attach presumed SID prefix
    if (debug_radio) {
      if (message_buffer_length) message_buffer_length--; // chop NL
      message_buffer_length+=
	snprintf(&message_buffer[message_buffer_length],
		 message_buffer_size-message_buffer_length,
		 ", FEC OK : sender SID=%02x%02x%02x%02x%02x%02x*\n",
		 packet_data[0],packet_data[1],packet_data[2],
		 packet_data[3],packet_data[4],packet_data[5]);
    }
    
    if (monitor_mode)
      {
	char monitor_log_buf[1024];
	snprintf(monitor_log_buf,sizeof(monitor_log_buf),
		 "CSMA Data frame: frame len=%d, FEC OK",		 
		 packet_bytes);	
	monitor_log(sender_prefix,NULL,monitor_log_buf);
      }
    return 0;
  } else {
    if (debug_radio) {
      if (message_buffer_length) message_buffer_length--; // chop NL
      message_buffer_length+=
	snprintf(&message_buffer[message_buffer_length],
		 message_buffer_size-message_buffer_length,
		 ", FEC FAIL (rs_error_count=%d)\n",
		 rs_error_count);
    }
    return -1;
  }

}

int radio_ready()
{
  if (radio_get_type()==RADIO_RFD900) return 1;
  else return hf_radio_ready();
}
