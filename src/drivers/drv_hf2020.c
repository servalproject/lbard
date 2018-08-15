
/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: HF2020,"hf2020","Clover 2020 HF modem",hf2020_radio_detect,hf2020_serviceloop,hf2020_receive_bytes,hf2020_send_packet,hf2020_my_turn_to_send,20

The Clover modem uses 0x80 0xXX commands to do various things.
0x80 0x06 - Immediate abort
0x80 0x07 - Polite hangup of call
0x80 0x09 - Hardware reset
0x80 0x80 - Enable clover mode
0x80 0x0A - Transmit CW ID when possible
0x80 0x10 + [0x80 0xCC]x(1-8) + [0x80 0x00] to call station CC...
0x80 0x59 - Enable echo-as-sent to use for flow control (count bytes sent). The buffer is 1KB, and for high speed, >255 bytes need to be in the buffer.
0x80 0x65 + 0x80 0x02 - Set RS error correction rate to 90%
0x80 0xfe + 0x80 0x00 + 0x80 0x00 - Set clover mask to recommended default
0x80 0x51 - Enable channel statistics reports
0x80 0x56 - Enable expanded link state reports
0x80 0x5A - Enable connect status and related messages
0x80 0x6A + 0x80 0x07 - Set serial port to 57600 until next reset (if we decide to use this feature. We would need to improve the auto-detect to check 57600 as well)
0x80 0x6F + 0x80 0x01 - Switch to AT command mode.

Characters 0x80 and 0x81 for TX or RX must be escaped with 0x81 in front.
0x80 0x30 from modem means following bytes (if any) are RX
0x80 0x31 from modem means following bytes (if any) are TX (this is the echo back enabled by 0x80 0x59)
0x80 0x72 returns link info: byte 2 = SNR in 0.5db units, byte 5 = data rate in bytes per second
0x80 0x27 - Someone is calling us.
0x80 0x20 + peer + 0x80 0x00 - Linked to someone
0x91 0x33 - Next bytes are from secondary port
0x91 0x31 - Next bytes are from primary port

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

extern int previous_state;

int send80cmd(int fd,unsigned char c)
{
  unsigned char cmd[2];
  cmd[0]=0x80; cmd[1]=c;
  write_all(fd,cmd,2);

  printf("0x80%02x written. Waiting for reply.\n",c);
  return 0;
}

int hf2020_my_turn_to_send(void)
{
  if (hf2020_ready_test())
    return hf_radio_check_if_ready();
  else
    return 0;
}

int hf2020_ready_test(void)
{
  int isReady=1;
  
  if (hf_state!=HF_ALELINK) isReady=0;
  if (ale_inprogress) isReady=0;
  // XXX  if (!barrett_link_partner_string[0]) isReady=0;

  return isReady; 
}

int hf2020_initialise(int fd, unsigned int product_id)
{
  fprintf(stderr,"Detected a Clover HF modem, model ID %04x\n",
	  product_id);

  // Reset takes a long time, so don't do anything now.
  // send80cmd(fd,0x09); // Hardware reset
  
  send80cmd(fd,0x06); // Stop whatever we are doing
  send80cmd(fd,0x80); // Switch to clover mode
  send80cmd(fd,0x65); send80cmd(fd,0x02); // Set FEC mode to "FAST"
  send80cmd(fd,0x59); // Enable echo-as-sent
  send80cmd(fd,0xfe); send80cmd(fd,0x00); send80cmd(fd,0x00); // use default CRC mask
  send80cmd(fd,0x51); // enable channel status reports
  send80cmd(fd,0x56); // enable expanded link state reports
  send80cmd(fd,0x5a); // enable even more reports

  // To be able to talk to the Barrett radio via AT commands,
  // we need to enable the secondary serial port.  This port
  // should always be at 9600, regardless of whether the head
  // has the RS232 port set to 9600 or 115200.  This makes life
  // a little easier for us.
  
  // Enable secondary port
  send80cmd(fd,0x69);
  send80cmd(fd,0x04);
  // Switch to secondary serial port for TX
  send80cmd(fd,0x34);

  // Send AIATBL command to Barrett modem to get ALE call list
  // (for both peers, and also so we know our own ID, so that we can configure it in the clover modem)
  write_all(fd,"\r\nAIATBL\r\n",10);
  
  return 1;
}

int hf2020_serviceloop(int serialfd)
{
  switch(hf_state) {

  case HF_DISCONNECTED:
  
    // The call requested has to abort, the radio is receiving a another call
    if (previous_state==HF_CALLREQUESTED){
      printf("XXX Aborting call\n");
    }
    
    // Wait until we are allowed our first call before doing so
    if (time(0)<last_outbound_call) return 0;    
    
    // Currently disconnected. If the current time is later than the next scheduled
    // If the radio is not receiving a message
    // call-out time, then pick a hf station to call

    if ((ale_inprogress==0)&&(hf_link_partner==-1)&&(hf_station_count>0)&&(time(0)>=hf_next_call_time)) {
      int next_station = hf_next_station_to_call();
      if (next_station>-1) {
	// XXX Try to connect
      }
    }
    else if (hf_link_partner>-1) {
      // If we are connected to someone, then mark us as being in-call
      // XXX This probably needs to change for the 2020
      hf_state=HF_ALELINK;
    }
    else if (time(0)!=last_link_probe_time) { //once a second
      // XXX - Probe to see if we are still connected
      last_link_probe_time=time(0);
    }
  
    break;

  case HF_CALLREQUESTED: //2
    // Probe periodically with AILTBL to get link table, because the modem doesn't
    // preemptively tell us when we get a link established
    if (time(0)!=last_link_probe_time)  { //once a second
      //write(serialfd,"AILTBL\r\n",8);
      last_link_probe_time=time(0);
    }
    if (ale_inprogress==2){
      printf("Another radio is calling. Marking this sent call disconnected\n");
      hf_state=HF_DISCONNECTED;
    }
    if (time(0)>=hf_next_call_time){ //no reply from the called station
      hf_state = HF_DISCONNECTED;
      printf("Make the call disconnected because of no reply\n");
    }
    break;
    
  case HF_CONNECTING: //3
    printf("XXX HF_CONNECTING\n");
    break;

  case HF_ALELINK: //4
		
    if (time(0)!=last_link_probe_time)  { //once a second
      // XXX - Probe to check that we are still connected
      last_link_probe_time=time(0);
    }
    
    if (previous_state!=HF_ALELINK){
      fprintf(stderr,"Radio linked with %s (station #%d), I will send a packet in %ld seconds\n",
	      "(XXX unknown)",hf_link_partner,
      hf_next_packet_time-time(0));
    }

    
    break;

  case HF_DISCONNECTING: //5
    
    printf("XXX Aborting the current established ALE link\n");

    hf_state=HF_DISCONNECTED;
    
    break;

  case HF_ALESENDING: //6
    // This state is managed in the hf2020_send_packet function
    break;

  case HF_RADIOCONFUSED: //7
    // We entenred a case where the radio is confused
    printf("XXX HF Radio is confused. Marking as disconnected.\n");
    hf_state=HF_DISCONNECTED;
    
    break;

  default:
		
    break;
  }

  if (previous_state!=hf_state){
    fprintf(stderr,"\nClover 2020 modem changed to state %s\n",hf_state_name(hf_state));
    previous_state=hf_state;
  }
  return 0;
}

int hf2020_process_reply(char *l)
{
  return 0;
}

int esc80=0;
int esc91=0;
int fromSecondary=0;
int fromTXEcho=0;

// For gathering 80xx status message responses
int status80BytesRemaining=0;
int status80len=0;
unsigned char status80[256];

int hf2020_receive_bytes(unsigned char *bytes,int count)
{
  int i;
  for(i=0;i<count;i++) {
    unsigned char b=bytes[i];
    if (esc80) {
      esc80=0;
      printf("saw 80 %02x\n",b);
      if (status80BytesRemaining) {
	status80[status80len++]=b;
	status80BytesRemaining--;
	// Check for end of null-terminated string in reply
	if ((status80BytesRemaining<0)&&(b==0x00))
	  {	    
	    status80BytesRemaining=0;
	    dump_bytes(stdout,"Collected 80xx status variable-length message",status80,status80len);
	  }
	// Check for end of fixed-length reply
	if (!status80BytesRemaining) {
	  dump_bytes(stdout,"Collected 80xx status fixed-length message",status80,status80len);
	}
      } else {
	switch(b) {
	case 0x06: case 0x09:
	  // Various status messages we can safely ignore
	  break;
	case 0x20: case 0x21: case 0x22:
	case 0x26: case 0x28: case 0x2a:
	case 0x2b: case 0x2c: case 0x2d:
	case 0x2e:
	  // Various variable-length replies
	  status80[0]=b; status80len=1;
	  // (Negative bytes-remaining count indicates variable length
	  // terminated by 80 00).
	  status80BytesRemaining=-1; break;
	case 0x23: case 0x24:
	  // Link disconnect notification 
	  hf_state=HF_DISCONNECTED;
	  status80[0]=b; status80len=1; status80BytesRemaining=1; break;
	case 0x30:
	  // Bytes are RX from modem
	  fromSecondary=0; fromTXEcho=0; break;
	case 0x31:
	  // Bytes are TX echos from modem
	  fromSecondary=0; fromTXEcho=1; break;
	case 0x32:
	  // Bytes are from secondary port
	  fromSecondary=1; break;
	case 0x70:
	  // Channel spectra (8 bytes)
	  status80[0]=b; status80len=1; status80BytesRemaining=8; break;
	case 0x72:
	  // Channel statistics (7 bytes for both ends)
	  status80[0]=b; status80len=1; status80BytesRemaining=7*2; break;
	default:
	  printf("Saw unknown status message: 80 %02x\n",b);
	}
      }
    } else if (esc91) {
      esc91=0;
      if (b==0x33) fromSecondary=1;
      if (b==0x31) fromSecondary=0;
    } else {      
      switch(b) {
      case 0x80: esc80=1; break;
      case 0x91: esc91=1; break;
      default:
	if (fromSecondary) {
	  // Barrett modem output
	  hfbarrett_receive_bytes(&b,1);
	} else {
	  // Clover modem output
	}
      }
    }
  }
  return 0;
}

int hf2020_send_packet(int serialfd,unsigned char *out, int len)
{
  return 0;
}

int hf2020_extract_product_id(unsigned char *buffer,int count)
{
  int product_id=0;
  for(int i=0;i<(count-5);i++) {
    if ((buffer[i]==0x80)
	&&(buffer[i+1]==0x7B)
	&&(buffer[i+2]==0x80)
	&&(buffer[i+4]==0x80)) {      
      product_id=(buffer[i+3]<<8)+buffer[i+5];
      break;
    }
  }
  return product_id;
}

int hf2020_radio_detect(int fd)
{
  // We require a serial port
  if (fd==-1) return -1;

  // Ask for product ID
  serial_setup_port_with_speed(fd,9600);
  send80cmd(fd,0x7B);

  unsigned char buffer[1024];
  usleep(100000);
  int count=read_nonblock(fd,buffer,1024);
  
  unsigned int product_id=hf2020_extract_product_id(buffer,count);

  if (!product_id) {
    serial_setup_port_with_speed(fd,115200);
    send80cmd(fd,0x7B);
    usleep(100000);
    count=read_nonblock(fd,buffer,1024);
    product_id=hf2020_extract_product_id(buffer,count);
  }

  if (product_id) {
    radio_set_type(RADIOTYPE_HF2020);
    return hf2020_initialise(fd,product_id);    
  }

  return -1;
}
