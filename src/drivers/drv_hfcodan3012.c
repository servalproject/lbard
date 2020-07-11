
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

int hfcodan3012_initialise(int serialfd)
{
  char cmd[1024];
  fprintf(stderr,"Initialising Codan HF 3012 modem with id '%s'...\n",hfselfid?hfselfid:"<not set>");
  snprintf(cmd,1024,"at&i=%s\r\n",hfselfid?hfselfid:"1");
  write_all(serialfd,cmd,strlen(cmd));
  fprintf(stderr,"Set HF station ID in modem to '%s'\n",hfselfid?hfselfid:"1");
  
}

int hfcodan3012_radio_detect(int fd)
{
  // We require a serial port
  if (fd==-1) return -1;

  serial_setup_port_with_speed(fd,9600);
  // Abort any help display, incase we are in one
  write_all(fd,"q",1);
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
  if (!strstr(response_buffer,"CODAN Ltd.")) return -1;
  dump_bytes(stderr,"Response from serial port was:\n",response_buffer,count);
  
  // Get model number etc
  write_all(fd,"ati1\r\n",5);
  usleep(300000);
  count = read(fd, response_buffer, sizeof response_buffer);
  if (count>=0&&count<sizeof(response_buffer))
    response_buffer[count]=0;
  else
    response_buffer[sizeof(response_buffer)-1]=0;
  char *model_name=&response_buffer[0];
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
  char cmd[1024];

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
	char cmd[1024];
	snprintf(cmd,1024,"atd%s\r\n",remoteid);
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
  int channel,caller,callee,day,month,hour,minute;
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

unsigned char packet_rx_buffer[256];
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
	  if (rx_len<256) {
	    packet_rx_buffer[rx_len++]='!';
	  }
	  break;
	case '!': // end of packet

	  // Reset our hangup timeout
	  call_timeout=time(0)+120;
	  if (saw_packet(packet_rx_buffer,rx_len,0,
			 my_sid_hex,prefix,
			 servald_server,credential)) {
	  } else {
	  }
	  rx_len=0;

	  break;
	case 'C': // clear RX buffer
	  rx_len=0;
	  break;
	default:
	  break;
	}
	rx_esc=0;
      } else {
	// Not in escape mode
	if (bytes[i]=='!') rx_esc=1;
	else {
	  if (rx_len<256) {
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
    serial_errors++;
    return -1;
  } else {
    serial_errors=0;
    return 0;
  }
  
  return 0;
}

