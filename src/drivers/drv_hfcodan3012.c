
/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: HFCODAN3012,"hfcodan3012","Codan HF with 3012 Data Modem",hfcodan3012_radio_detect,hfcodan3012_serviceloop,hfcodan3012_receive_bytes,hfcodan3012_send_packet,hf_radio_check_if_ready,10

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
#include <ctype.h>

#include "sync.h"
#include "lbard.h"
#include "hf.h"
#include "radios.h"

extern char *hfselfid;
extern char *hfcallplan;

int hfcallplan_pos=0;

int hfselfidseen=0;

int tx_seq=0;
int last_tx_reflected_seq;
int rx_seq=0;


int hfcodan3012_initialise(int serialfd)
{
  char cmd[1024];
  fprintf(stderr,"Initialising Codan HF 3012 modem with id '%s'...\n",hfselfid?hfselfid:"<not set>");
  snprintf(cmd,1024,"at&i=%s\r\n",hfselfid?hfselfid:"1");
  write_all(serialfd,cmd,strlen(cmd));
  fprintf(stderr,"Set HF station ID in modem to '%s'\n",hfselfid?hfselfid:"1");

  snprintf(cmd,1024,"at&k=3\r\n");
  write_all(serialfd,cmd,strlen(cmd));
  fprintf(stderr,"Enabling hardware flow control.\n");

  // Slow message rate, so that we don't have overruns all the time,
  // and so that we don't end up with lots of missed packets which messes with the
  // sync algorithm
  message_update_interval = 1000;
  return 0;
}

int hfcodan3012_radio_detect(int fd)
{
  // We require a serial port
  if (fd==-1) return -1;

  serial_setup_port_with_speed(fd,9600);
  // Abort any help display, incase we are in one
  write_all(fd,"q",1);
  // drop out of online mode if required
  usleep(1500000);
  write_all(fd,"+++",3);
  usleep(1100000);
  // Ask for copyright notice
  write_all(fd,"ati2\r\n",5);
  usleep(300000);
  unsigned char response_buffer[1024];
  int count = read(fd, response_buffer, sizeof response_buffer);
  if (count>=0&&count<sizeof(response_buffer))
    response_buffer[count]=0;
  else
    response_buffer[sizeof(response_buffer)-1]=0;
  // Look for Codan name in copyright. If not present, then not a Codan HF modem 
  if (!strstr((char *)response_buffer,"CODAN Ltd.")) return -1;
  dump_bytes(stderr,"Response from serial port was:\n",response_buffer,count);
  
  // Get model number etc
  write_all(fd,"ati1\r\n",5);
  usleep(300000);
  count = read(fd, response_buffer, sizeof response_buffer);
  if (count>=0&&count<sizeof(response_buffer))
    response_buffer[count]=0;
  else
    response_buffer[sizeof(response_buffer)-1]=0;
  char *model_name=(char *)&response_buffer[0];
  while(*model_name&&*model_name!='\n') model_name++;
  if (*model_name) model_name++;
  char *m2=model_name+1;
  while(*m2&&(*m2>=' ')) m2++;
  *m2=0;
  if (!strcmp("3012E",model_name)) {
    radio_set_type(RADIOTYPE_HFCODAN3012);
    hfcodan3012_initialise(fd);    
    return 1;
  } else {
    fprintf(stderr,"Unknown/unsupported Codan Data Modem type '%s' detected. Aborting.\n",model_name);
    exit(-2);
  }
  
  return -1;
}

time_t call_timeout=0;


int last_hf_state=0;

int hfcodan3012_serviceloop(int serialfd)
{
  // XXX DEBUG show when state changes
  if (hf_state!=last_hf_state) {
    fprintf(stderr,"Codan 3012 modem is in state %d, callplan='%s':%d\n",hf_state,hfcallplan,hfcallplan_pos);
    last_hf_state=hf_state;
  }

  // HF call plan and ID only become available after auto-detection, so we have to call initialise again
  if (hfselfid&&!hfselfidseen) {
    hfcodan3012_initialise(serialfd);
    hfselfidseen=1;
  }
  
  
  switch(hf_state) {
  case HF_DISCONNECTED:
    if (hfcallplan) {
      int n;
      char remoteid[1024];
      int f=sscanf(&hfcallplan[hfcallplan_pos],
		   "call %[^,]%n",
		   remoteid,&n);
      if (f==1) {
	hfcallplan_pos+=n;
	fprintf(stderr,"Calling station '%s'\n",remoteid);
	char cmd[2048];
	snprintf(cmd,2048,"atd%s\r\n",remoteid);
	write_all(serialfd,cmd,strlen(cmd));
	call_timeout=time(0)+300;
	hf_state=HF_CALLREQUESTED;
      } else {
	fprintf(stderr," remoteid='%s', n=%d, f=%d\n",remoteid,n,f);
      }
      
      while (hfcallplan[hfcallplan_pos]==',') hfcallplan_pos++;
      if (!hfcallplan[hfcallplan_pos]) hfcallplan_pos=0;
    }    
    break;
  case HF_CALLREQUESTED:
    if (time(0)>=call_timeout) hf_state=HF_DISCONNECTED;
    break;
  case HF_ANSWERCALL:
    write_all(serialfd,"ata\r\n",5);
    hf_state=HF_CONNECTING;
    call_timeout=300;
    break;
  case HF_CONNECTING:
    // wait for CONNECT or NO CARRIER message
    break;
  case HF_DATALINK:
    // Modem is connected
    write_all(serialfd,"ato\r\n",5);
    hf_state=HF_DATAONLINE;
    call_timeout=time(0)+120;
    break;
  case HF_DATAONLINE:
    // Mode is online.  Do nothing but indicate that the modem is
    // ready to process packets
    if (time(0)>call_timeout) {
      // Nothing for too long, so hang up
      sleep(2);
      write_all(serialfd,"+++",3);
      sleep(2);
      write_all(serialfd,"ath0\r\n",5);
      hf_state=HF_DISCONNECTED;
    }
    break;
  case HF_DISCONNECTING:
    break;
  default:
    break;
  }
  
  return 0;
}

int hfcodan3012_process_line(char *l)
{
  fprintf(stderr,"Saw line from modem: '%s'\n",l);
  if (!strncmp(l,"NO CARRIER",10)) {
    hf_state=HF_DISCONNECTED;
  }
  if (!strncmp(l,"NO ANSWER",9)) {
    hf_state=HF_DISCONNECTED;
  }
  if (!strncmp(l,"CONNECT",10)) {
    hf_state=HF_DATAONLINE;
    // And announce ourselves as ready for the first packet of the connection
    hf_radio_mark_ready();
    call_timeout=time(0)+120;
  }
  if (!strncmp(l,"RING",4)) {
    fprintf(stderr,"Saw incoming call. Answering.\n");
    hf_state=HF_ANSWERCALL;
  }
  return 0;
}

unsigned char packet_rx_buffer[512];
int rx_len=0;
int rx_esc=0;


int hfcodan3012_receive_bytes(unsigned char *bytes,int count)
{ 
  int i;
  if (hf_state==HF_DATAONLINE) {
    // Online mode, so decode packets
    for(i=0;i<count;i++) {      
      
      if (rx_esc) {
	switch(bytes[i]) {
	case '.': // escaped !
	  if (rx_len<500) {
	    packet_rx_buffer[rx_len++]='!';
	  }
	  break;
	case '!': // end of packet

	  // Reset our hangup timeout
	  call_timeout=time(0)+120;
	  last_tx_reflected_seq=packet_rx_buffer[1];
	  rx_seq=packet_rx_buffer[0];

	  fprintf(stderr,"Saw packet #$%02x, reflecting reception of packet #$%02x from us.\n",
		  rx_seq,last_tx_reflected_seq);
	  
	  if (saw_packet(&packet_rx_buffer[2],rx_len-2,0,
			 my_sid_hex,prefix,
			 servald_server,credential)) {
	  } else {
	  }
	  rx_len=0;

	  break;
	case 'C': // clear RX buffer
	  rx_len=0;
	  break;
	case 'D': // Check for data packet
	  {
	    int payload_ofs=packet_rx_buffer[0]+(packet_rx_buffer[1]<<8)+(packet_rx_buffer[2]<<16)+(packet_rx_buffer[3]<<24);
	    int encoded_len=packet_rx_buffer[4]+(packet_rx_buffer[5]<<8);
	    if (encoded_len==(rx_len-4-2)) {
	      fprintf(stderr,"Saw %d bytes of data for bundle offset %d in a data packet.\n",
		      encoded_len,payload_ofs);
	      // Ah, yes, now we should actually record that we received the data. That would be a good idea.
	      // Here its a bit interesting, because we rely on whatever bundle was referenced in the main packet.
	      // We don't currently keep a sense of "last rx bundle piece", but we need to for this.
	      if (last_partial_number>-1) {
		record_bundle_piece(last_partial_number,
				    0, // XXX ugly: we assume it has come from peer #0, because we should have only one peer
				    payload_ofs,encoded_len,0,0,&packet_rx_buffer[6],
				    prefix,servald_server,credential);
	      }
	    } else {
	      fprintf(stderr,"CORRUPT DATA PACKET: %d bytes of data for bundle offset %d in a data packet, but rx_len=%d.\n",
		      encoded_len,payload_ofs,rx_len);
	      dump_bytes(stderr,"Received data",packet_rx_buffer,rx_len);
	    }	    
	    rx_len=0;
	  }
	default:
	  break;
	}
	rx_esc=0;
      } else {
	// Not in escape mode
	if (bytes[i]=='!') rx_esc=1;
	else {
	  if (rx_len<258) {
	    packet_rx_buffer[rx_len++]=bytes[i];
	  }
	}
      }
    }
    
  } else {
    // Modem command mode
    for(i=0;i<count;i++) {
      if (bytes[i]==13||bytes[i]==10) {
	hf_response_line[hf_rl_len]=0;
	if (hf_rl_len) hfcodan3012_process_line(hf_response_line);
	hf_rl_len=0;
      } else {
	if (hf_rl_len<1024) hf_response_line[hf_rl_len++]=bytes[i];
      }
    }
  }
  
  return 0;
}

int hfcodan3012_send_packet(int serialfd,unsigned char *out, int len)
{
  // Now escape any ! characters, and append !! to the end to mark the 
  // packet boundary. I.e., very similar to what we do with the RFD900.

  unsigned char escaped[2+len*2+2];
  int elen=0;
  int i;

  if (hf_state!=HF_DATAONLINE) {
    //    fprintf(stderr,"Ignoring packet while not in DATAONLINE state.\n");
    // Silently ignore sending
    return 0;
  }

  // Sometimes the ! gets eaten here. Solution is to
  // send a non-! character first, so that even if !-mode
  // is set, all works properly.  this will also stop us
  // accidentally doing !!, which will send a packet.
  write(serialfd,"C!C",3);

  // Then sequence # and the last sequence # we have seen from the remote
  if (tx_seq==0x21) write(serialfd,"!.",2); else {
    char s=tx_seq; write(serialfd,&s,1);
  }
  if (rx_seq==0x21) write(serialfd,"!.",2); else {
    char s=rx_seq; write(serialfd,&s,1);
  }
  tx_seq++;

  // Modulate TX rate based on the number of outstanding packets we have
  int packets_unacknowledged=(tx_seq&0xff)-last_tx_reflected_seq;
  if (packets_unacknowledged<0) packets_unacknowledged+=256;
  if (packets_unacknowledged>32) packets_unacknowledged=32;
  if (packets_unacknowledged<3) message_update_interval=250*packets_unacknowledged;
  else message_update_interval=500*packets_unacknowledged;
  fprintf(stderr,"Sending packet #$%02x, len=%d, packet interval=%d ms, unackd packets=%d\n",
	  tx_seq,len,message_update_interval,packets_unacknowledged);
  
  // Then stuff the escaped bytes to send
  for(i=0;i<len;i++) {
    if (out[i]=='!') {
      escaped[elen++]='!'; escaped[elen++]='.';
    } else escaped[elen++]=out[i];
  }
  // Finally include TX packet command
  escaped[elen++]='!'; escaped[elen++]='!';
  if (debug_radio_tx) {
    dump_bytes(stdout,"sending packet",escaped,elen);    
  }  

  if (write_all(serialfd,escaped,elen)==-1) {
    fprintf(stderr,"Serial error. Returning early.\n");
    serial_errors++;
    return -1;
  } else {
    serial_errors=0;
  }

  /* 
     Follow up with a data-only packet with very light encapsulation, so that we 
     can make more effective use of the modem interface.
     It will always be payload bytes from the tx_bundle of peer 0, since we should
     only have one peer on the link.
     
  */
  fprintf(stderr,"peer_count=%d, peer_records[0]->tx_bundle=%d\n",peer_count,peer_count?peer_records[0]->tx_bundle:-1);
  if ((peer_count>0)&&(peer_records[0]->tx_bundle>0)&&cached_body)
    {
      unsigned char data_packet[1024];
      int ofs=0;

      // XXX We assume bundle cache has already been primed
      int body_offset=peer_records[0]->tx_bundle_body_offset;
      int bytes_to_send=cached_body_len-body_offset;
      // 256 bytes seems to fill the buffer up and results in lost characters
      if (bytes_to_send>192) bytes_to_send=192;

      if (bytes_to_send>0) {
	fprintf(stderr,"Sending pure data packet with %d bytes\n",bytes_to_send);
	// Write offset
	
	data_packet[ofs++]=(body_offset>>0)&0xff;
	data_packet[ofs++]=(body_offset>>8)&0xff;
	data_packet[ofs++]=(body_offset>>16)&0xff;
	data_packet[ofs++]=(body_offset>>24)&0xff;
	// Length of data
	data_packet[ofs++]=(bytes_to_send>>0)&0xff;
	data_packet[ofs++]=(bytes_to_send>>8)&0xff;
	// Write bytes
	for(int j=0;j<bytes_to_send;j++) {
	  if (cached_body[body_offset+j]!='!')
	    data_packet[ofs++]=cached_body[body_offset+j];
	  else {
	    data_packet[ofs++]='!'; data_packet[ofs++]='.';
	  }
	}
	
	// And end of packet marker
	data_packet[ofs++]='!';
	data_packet[ofs++]='D';
	
	dump_bytes(stderr,"Data packet to send",data_packet,ofs);
	
	if (write_all(serialfd,data_packet,ofs)==-1) {
	  serial_errors++;
	  return -1;
	} else {
	  serial_errors=0;
	  peer_records[0]->tx_bundle_body_offset+=bytes_to_send;	
	  return 0;
	}
      }
      
    }
  
  
  return 0;
}

