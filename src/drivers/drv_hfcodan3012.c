
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

	  fprintf(stderr,"Saw packet #$%02x, reflecting reception of packet #$%02x from us (last_partial_number=%d)\n",
		  rx_seq,last_tx_reflected_seq,last_partial_number);
	  
	  if (saw_packet(&packet_rx_buffer[2],rx_len-2,0,
			 my_sid_hex,prefix,
			 servald_server,credential)) {
	  } else {
	  }
	  fprintf(stderr,"  After parsing, last_partial_number=%d\n",last_partial_number);
	  rx_len=0;

	  break;
	case 'C': // clear RX buffer
	  rx_len=0;
	  break;
	case 'D': // Check for data packet
	  {
	    int payload_ofs=packet_rx_buffer[0]+(packet_rx_buffer[1]<<8)+(packet_rx_buffer[2]<<16)+(packet_rx_buffer[3]<<24);
	    int payload_len=packet_rx_buffer[4]+(packet_rx_buffer[5]<<8);
	    int is_manifest=payload_ofs&0x80000000;
	    int is_endpiece=payload_ofs&0x40000000;
	    payload_ofs&=0x3fffffff;
	    if (payload_len==(rx_len-4-2-6-8)) {
	      // Ah, yes, now we should actually record that we received the data. That would be a good idea.
	      // Here its a bit interesting, because we rely on whatever bundle was referenced in the main packet.
	      // We don't currently keep a sense of "last rx bundle piece", but we need to for this.
	      char bid_prefix[16];
	      snprintf(bid_prefix,16,"%02x%02x%02x%02x%02x%02x",
		       packet_rx_buffer[6],packet_rx_buffer[7],packet_rx_buffer[8],
		       packet_rx_buffer[9],packet_rx_buffer[10],packet_rx_buffer[11]);
	      unsigned char *bid_prefix_bin=&packet_rx_buffer[6];
	      int for_me=1;
	      char *peer_prefix=peer_records[0]->sid_prefix;
	      long long version=0;
	      for(int j=0;j<8;j++) { version=version<<8; version|=packet_rx_buffer[12+j]; } 
	      fprintf(stderr,"Saw %d bytes of data for %s*/%lld offset %d in a data packet, M=%d, E=%d.\n",
		      payload_len,bid_prefix,version,
		      payload_ofs,is_manifest?1:0,is_endpiece?1:0);
	      saw_piece(peer_prefix,for_me,bid_prefix,bid_prefix_bin,
			version, payload_ofs, payload_len, is_endpiece,
			is_manifest,&packet_rx_buffer[4+2+6+8],
			prefix,servald_server,credential);
	    } else {
	      fprintf(stderr,"CORRUPT DATA PACKET: %d bytes of data for bundle offset %d in a data packet, but rx_len=%d.\n",
		      payload_len,payload_ofs,rx_len);
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

int data_packet_ofs=0;
int data_packet_manifestP=1;

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

      // Start sending blocks in order, including the manifest, so tht we can get bundles through
      // faster.

      // We send linearly through both manifest and body, since the HF modem is, in theory at least,
      // providing reliable transport.
      if (data_packet_manifestP&&data_packet_ofs>=cached_manifest_encoded_len) {
	data_packet_ofs=0; data_packet_manifestP=0;
      }
      if ((!data_packet_manifestP)&&data_packet_ofs>=cached_body_len) {
	data_packet_ofs=0; data_packet_manifestP=1;
      }
      // Honour hard lower limits, so that we don't waste time sending content the other
      // side has already acknowledged
      // XXX Ideally we should also honour current sending bitmap
      if (data_packet_manifestP) {
	if (data_packet_ofs<peer_records[0]->tx_bundle_manifest_offset_hard_lower_bound)
	  data_packet_ofs=peer_records[0]->tx_bundle_manifest_offset_hard_lower_bound;
      } else {
	if (data_packet_ofs<peer_records[0]->tx_bundle_body_offset_hard_lower_bound)
	  data_packet_ofs=peer_records[0]->tx_bundle_body_offset_hard_lower_bound;
      }
      
      // XXX We assume bundle cache has already been primed
      int end_piece=0;
      int bytes_to_send=cached_body_len-data_packet_ofs;
      if (data_packet_manifestP) bytes_to_send=cached_manifest_encoded_len-data_packet_ofs;

      // 256 bytes seems to fill the buffer up and results in lost characters
      if (bytes_to_send>128) bytes_to_send=128;

      if (data_packet_manifestP&&((data_packet_ofs+bytes_to_send)>=cached_manifest_encoded_len)) end_piece=1;
      if ((!data_packet_manifestP)&&((data_packet_ofs+bytes_to_send)>=cached_body_len)) end_piece=1;

      if (bytes_to_send>0) {
	fprintf(stderr,"Sending pure data packet of %s with %d bytes: %d..%d, M=%d\n",
		bid_of_cached_bundle,
		bytes_to_send,
	        data_packet_ofs,data_packet_ofs+bytes_to_send-1,data_packet_manifestP);
	// Write offset
	
	data_packet[ofs++]=(data_packet_ofs>>0)&0xff;
	data_packet[ofs++]=(data_packet_ofs>>8)&0xff;
	data_packet[ofs++]=(data_packet_ofs>>16)&0xff;
	data_packet[ofs++]=((data_packet_ofs>>24)&0x3f)|(data_packet_manifestP?0x80:0x00)|(end_piece?0x40:0x00);
	// Length of data
	data_packet[ofs++]=(bytes_to_send>>0)&0xff;
	data_packet[ofs++]=(bytes_to_send>>8)&0xff;
	// BID prefix
	sscanf(bid_of_cached_bundle,"%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
	       &data_packet[ofs+0],&data_packet[ofs+1],&data_packet[ofs+2],
	       &data_packet[ofs+3],&data_packet[ofs+4],&data_packet[ofs+5]);
	ofs+=6;
	// Version
	for(int j=0;j<8;j++) data_packet[ofs++]=(cached_version>>(j*8))&0xff;	
	
	// Write bytes
	if (data_packet_manifestP) {
	  for(int j=0;j<bytes_to_send;j++) {
	    if (cached_manifest_encoded[data_packet_ofs+j]!='!')
	      data_packet[ofs++]=cached_manifest_encoded[data_packet_ofs+j];
	    else {
	      data_packet[ofs++]='!'; data_packet[ofs++]='.';
	    }
	  }
	} else {       
	  for(int j=0;j<bytes_to_send;j++) {
	    if (cached_body[data_packet_ofs+j]!='!')
	      data_packet[ofs++]=cached_body[data_packet_ofs+j];
	    else {
	      data_packet[ofs++]='!'; data_packet[ofs++]='.';
	    }
	  }
	}
	
	// And end of packet marker
	data_packet[ofs++]='!';
	data_packet[ofs++]='D';

	data_packet_ofs+=bytes_to_send;
	
	dump_bytes(stderr,"Data packet to send",data_packet,ofs);
	
	if (write_all(serialfd,data_packet,ofs)==-1) {
	  serial_errors++;
	  return -1;
	} else {
	  serial_errors=0;
	  return 0;
	}
      }
      
    }
  
  
  return 0;
}

