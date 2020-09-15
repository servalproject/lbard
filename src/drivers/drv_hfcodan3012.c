
/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: HFCODAN3012,"hfcodan3012","Codan HF with 3012 Data Modem",hfcodan3012_radio_detect,hfcodan3012_serviceloop,hfcodan3012_receive_bytes,hfcodan3012_send_packet,hf_radio_check_if_ready,10

qwertyuiopasdfghjklzxcvbnm1234567890QWERTYUIOPASDFGHJKLZXCVBNM

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
int rx_seq=0xFF;

extern int serialfd;

#define PACKET_TIMEOUT 12000

int hfcodan3012_initialise(int serialfd)
{
  char cmd[1024];
  fprintf(stderr,"Initialising Codan HF 3012 modem with id '%s'...\n",hfselfid?hfselfid:"<not set>");
  snprintf(cmd,1024,"at&i=%s\r\n",hfselfid?hfselfid:"1");
  write_all(serialfd,cmd,strlen(cmd));
  fprintf(stderr,"Set HF station ID in modem to '%s'\n",hfselfid?hfselfid:"1");

  snprintf(cmd,1024,"at&K=3\r\n");
  write_all(serialfd,cmd,strlen(cmd));
  fprintf(stderr,"Enabling hardware flow control.\n");

  snprintf(cmd,1024,"at%%C0\r\n");
  write_all(serialfd,cmd,strlen(cmd));
  fprintf(stderr,"Disabling compression.\n");

  snprintf(cmd,1024,"at&M=5\r\n");
  write_all(serialfd,cmd,strlen(cmd));
  fprintf(stderr,"Selecting interactive mode.\n");

  // Slow message rate, so that we don't have overruns all the time,
  // and so that we don't end up with lots of missed packets which messes with the
  // sync algorithm
  // Actually, the sync algorithm gets upset if it sees responses to older packets
  // it seems. So we have to be a bit fancy and change the interval whenever we send a packet
  // to wait a long time before we try sending another (in case of lost packets), but resetting
  // the interval to 0 when we receive a packet from the other end.
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
time_t data_packet_timeout=0;

int last_hf_state=0;

int data_packet_ofs=0;
int data_packet_manifestP=1;

int rx_bytes=0;
int tx_bytes=0;

int log_tx(unsigned char *bytes,int count)
{
  char log[1024];
  snprintf(log,1024,"txlog.%d",getpid());
  FILE *f=fopen(log,"a");
  if (f) {
    for(int i=0;i<count;i++) {
      fprintf(f,"%08x %02x\n",tx_bytes++,bytes[i]);
    }
    fclose(f);
  }
  return 0;
}

void send_pure_data_packet(int pure_packet_max)
{
  /* 
     Follow up with a data-only packet with very light encapsulation, so that we 
     can make more effective use of the modem interface.
     It will always be payload bytes from the tx_bundle of peer 0, since we should
     only have one peer on the link.
     
  */
  if (!bid_of_cached_bundle) return;
  
  fprintf(stderr,">>> %s send_pure_data_packet: peer_count=%d, peer_records[0]->tx_bundle=%d, cached_body=%p\n",
	  timestamp_str(),peer_count,peer_count?peer_records[0]->tx_bundle:-1,cached_body);
  if ((peer_count>0)&&(peer_records[0]->tx_bundle>0))
    {
      unsigned char data_packet[1024];
      int ofs=0;
      
      prime_bundle_cache(peer_records[0]->tx_bundle,my_sid_hex,servald_server,credential);
      
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
      if (bytes_to_send>pure_packet_max) bytes_to_send=pure_packet_max;

      if (data_packet_manifestP&&((data_packet_ofs+bytes_to_send)>=cached_manifest_encoded_len)) end_piece=1;
      if ((!data_packet_manifestP)&&((data_packet_ofs+bytes_to_send)>=cached_body_len)) end_piece=1;

      printf(">>> %s There are %d eligible bytes for a pure data packet.\n",timestamp_str(),bytes_to_send);
      if (bytes_to_send>0) {
	fprintf(stderr,">>> %s Sending pure data packet of %s with %d bytes: %d..%d, M=%d\n",
		timestamp_str(),
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
	sscanf(bid_of_cached_bundle,"%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
	       &data_packet[ofs+0],&data_packet[ofs+1],&data_packet[ofs+2],
	       &data_packet[ofs+3],&data_packet[ofs+4],&data_packet[ofs+5],
	       &data_packet[ofs+6],&data_packet[ofs+7]);
	ofs+=8;
	// Version
	for(int j=7;j>=0;j--) data_packet[ofs++]=(cached_version>>(j*8))&0xff;	
	
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

	log_tx(data_packet,ofs);
	if (write_all(serialfd,data_packet,ofs)==-1) {
	  serial_errors++;
	  return;
	} else {
	  serial_errors=0;
	  return;
	}
      }
      
    }
}

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
	fprintf(stderr,">>> HFCALL %s Calling station '%s'\n",timestamp_str(),remoteid);
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
    if (time(0)>=call_timeout) {
      fprintf(stderr,">>> %s HFCALL disconnected: Call establishment timeout.\n",timestamp_str());
      hf_state=HF_DISCONNECTED;
    }
    break;
  case HF_ANSWERCALL:
    fprintf(stderr,">>> %s HFCALL incoming call seen.\n",timestamp_str());
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
    fprintf(stderr,">>> %s HFCALL going online.\n",timestamp_str());

    // Reset TX sequence management
    tx_seq=0;
    last_tx_reflected_seq=0xff;  // i.e., one less than 0x00, meaning no unacknowledged packets
    message_update_interval=500;
    next_message_update_time = gettime_ms() + message_update_interval;
    	  
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
      fprintf(stderr,">>> %s HFCALL disconnecting due to packet RX timeout.\n",timestamp_str());
      hf_state=HF_DISCONNECTED;
    } else {
      if (time(0)>=data_packet_timeout) {
	data_packet_timeout=time(0)+2;

	// Don't congest the link if it is >30 seconds since we last received a normal packet
	if ((call_timeout-time(0))>(120-30)) {
	  fprintf(stderr,">>> %s Getting ready to send a pure data packet.\n",timestamp_str());
	  send_pure_data_packet(200);
	} else
	  fprintf(stderr,">>> %s Not sending a pure data packet due to call timeout: call_timeout=T+%ld sec\n",
		  timestamp_str(),call_timeout-time(0));	  	
      }
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
int ack_seq_nybl=0;
int ack_seq_num=0;

int rx_byte_count=0;
time_t first_rx_time=0;

time_t last_rate_report_time=0;

int hfcodan3012_receive_bytes(unsigned char *bytes,int count)
{ 
  int i;  

  if (hf_state==HF_DATAONLINE) {
    // Online mode, so decode packets
 
    {
      char log[1024];
      snprintf(log,1024,"rxlog.%d",getpid());
      FILE *f=fopen(log,"a");
      if (f) {
	for(int i=0;i<count;i++) {
	  fprintf(f,"%08x %02x\n",rx_bytes++,bytes[i]);
	}
	fclose(f);
      }
    }
   
    rx_byte_count+=count;
    if (!first_rx_time) first_rx_time=time(0);
    else {
      if (time(0)!=last_rate_report_time) {
        last_rate_report_time=time(0);
	fprintf(stderr,"%d bytes received in %ld seconds.  Average %3.2f bytes per second.\n",
		rx_byte_count,time(0)-first_rx_time,
		rx_byte_count*1.0/(time(0)-first_rx_time));
      }
    }
    
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

	  printf(">>> %s Saw packet #$%02x, reflecting reception of packet #$%02x from us (last_partial_number=%d)\n",
		 timestamp_str(),
		 rx_seq,last_tx_reflected_seq,last_partial_number);

	  // Send ack to the other side
	  // !<2 chars 0x60-0x6f>
	  {
	    unsigned char ack[3]={'!',0x60+(rx_seq>>4),0x60+(rx_seq&0x0f)};
	    write_all(serialfd,ack,3);
	    log_tx(ack,3);
	    printf(">>> %s Sending ack of sequence #$%02x\n",timestamp_str(),rx_seq);
	  }
	  
	  int packets_unacknowledged=((tx_seq-1)&0xff)-last_tx_reflected_seq;
	  if (packets_unacknowledged<0) packets_unacknowledged+=256;
	  if (packets_unacknowledged>32) packets_unacknowledged=32;
	  message_update_interval=500+PACKET_TIMEOUT*packets_unacknowledged;
	  next_message_update_time = gettime_ms() + message_update_interval;
	  printf(">>> %s %d unacked packets remain. Interval set to %d ms\n",
		 timestamp_str(),packets_unacknowledged,message_update_interval);
  
	  if (saw_packet(&packet_rx_buffer[2],rx_len-2,0,
			 my_sid_hex,prefix,
			 servald_server,credential)) {
	  } else {
	  }
	  printf(">>> %s  After parsing, last_partial_number=%d\n",timestamp_str(),last_partial_number);
	  rx_len=0;

	  break;
	case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
	case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e: case 0x6f:
	  // First char of 2-byte ACK sequence number
	  if (ack_seq_nybl) {
	    ack_seq_nybl=0;
	    ack_seq_num|=(bytes[i]&0x0f);
	    //	    printf(">>> %s ACK low nybl from $%02x\n",timestamp_str(),bytes[i]);
	    last_tx_reflected_seq=ack_seq_num;
	    int packets_unacknowledged=((tx_seq-1)&0xff)-last_tx_reflected_seq;
	    if (packets_unacknowledged<0) packets_unacknowledged+=256;
	    if (packets_unacknowledged>32) packets_unacknowledged=32;
	    printf(">>> %s Saw ack of our packet $%02x (leaves %d unacknowledged)\n",timestamp_str(),ack_seq_num,packets_unacknowledged);
	    message_update_interval=0+PACKET_TIMEOUT*packets_unacknowledged;	    
	    next_message_update_time = gettime_ms() + message_update_interval;
	  } else {
	    //	    printf(">>> %s ACK high nybl from $%02x\n",timestamp_str(),bytes[i]);
	    ack_seq_nybl=1;
	    ack_seq_num=(bytes[i]&0x0f)<<4;	    
	  }
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
	    if (payload_len==(rx_len-4-2-8-8)) {
	      // Ah, yes, now we should actually record that we received the data. That would be a good idea.
	      // Here its a bit interesting, because we rely on whatever bundle was referenced in the main packet.
	      // We don't currently keep a sense of "last rx bundle piece", but we need to for this.
	      char bid_prefix[20];
	      snprintf(bid_prefix,20,"%02x%02x%02x%02x%02x%02x%02x%02x",
		       packet_rx_buffer[6],packet_rx_buffer[7],packet_rx_buffer[8],
		       packet_rx_buffer[9],packet_rx_buffer[10],packet_rx_buffer[11],
		       packet_rx_buffer[12],packet_rx_buffer[13]);		       
	      unsigned char *bid_prefix_bin=&packet_rx_buffer[6];
	      int for_me=1;
	      char *peer_prefix=peer_records[0]->sid_prefix;
	      long long version=0;
	      for(int j=0;j<8;j++) { version=version<<8; version|=packet_rx_buffer[14+j]; } 
	      fprintf(stderr,">>> %s Saw %d bytes of data for %s*/%lld offset %d in a data packet, M=%d, E=%d.\n",
		     timestamp_str(),
		     payload_len,bid_prefix,version,
		     payload_ofs,is_manifest?1:0,is_endpiece?1:0);
	      saw_piece(peer_prefix,for_me,bid_prefix,bid_prefix_bin,
			version, payload_ofs, payload_len, is_endpiece,
			is_manifest,&packet_rx_buffer[4+2+8+8],
			prefix,servald_server,credential);
	    } else {
	      fprintf(stderr,">>> %s CORRUPT DATA PACKET: %d bytes of data for bundle offset %d in a data packet, but rx_len=%d.\n",
		     timestamp_str(),
		     payload_len,payload_ofs,rx_len);
	      dump_bytes(stderr,"Received data",packet_rx_buffer,rx_len);
	    }	    
	    rx_len=0;
	  }
	default:
	  break;
	}
	if (!ack_seq_nybl) rx_esc=0;
      } else {
	// Not in escape mode
	if (bytes[i]=='!') { rx_esc=1; ack_seq_nybl=0; }
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
  log_tx((unsigned char *)"C!C",3);
  
  // Then sequence # and the last sequence # we have seen from the remote
  if (tx_seq==0x21) {
    write(serialfd,"!.",2);
    log_tx((unsigned char *)"!.",2);
  }
  else {
    char s=tx_seq; write(serialfd,&s,1);
    log_tx((unsigned char *)&s,1);
  }
  if (rx_seq==0x21) {
    write(serialfd,"!.",2);
    log_tx((unsigned char *)"!.",2);
  } else {
    char s=rx_seq; write(serialfd,&s,1);
    log_tx((unsigned char *)&s,1);
  }

  // Modulate TX rate based on the number of outstanding packets we have
  int packets_unacknowledged=((tx_seq-1)&0xff)-last_tx_reflected_seq;
  if (packets_unacknowledged<0) packets_unacknowledged+=256;
  if (packets_unacknowledged>32) packets_unacknowledged=32;
  packets_unacknowledged++; // The new packet is also unacknowledged
#if 0
  if (packets_unacknowledged<3) message_update_interval=250*packets_unacknowledged;
  else message_update_interval=500*packets_unacknowledged;
#else
  // Really try to avoid unacknowledged packets
  message_update_interval=500+PACKET_TIMEOUT*packets_unacknowledged;
#endif
  next_message_update_time = gettime_ms() + message_update_interval;
  
  printf(">>> %s Sending packet #$%02x, len=%d, packet interval=%d ms, unackd packets=%d\n",
	 timestamp_str(),
	 tx_seq,len,message_update_interval,packets_unacknowledged);

  tx_seq++;
  
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

  log_tx(escaped,elen);
  if (write_all(serialfd,escaped,elen)==-1) {
    fprintf(stderr,"Serial error. Returning early.\n");
    serial_errors++;
    return -1;
  } else {
    serial_errors=0;
  }

  // Don't congest the link if it is >30 seconds since we last received a normal packet
  if ((call_timeout-time(0))>(120-30)) {
    fprintf(stderr,">>> %s Getting ready to send a pure data packet.\n",timestamp_str());
    send_pure_data_packet(256);
  }
  
  return 0;
}

