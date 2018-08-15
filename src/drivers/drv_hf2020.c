
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

extern int serialfd;
extern char barrett_link_partner_string[1024];
extern int previous_state;

int send80cmd(int fd,unsigned char c)
{
  unsigned char cmd[2];
  cmd[0]=0x80; cmd[1]=c;
  write_all(fd,cmd,2);

  printf("80 %02x written.\n",c);
  return 0;
}

int send80stringz(int fd,char *s)
{
  for(int i=0;i<=strlen(s);i++)
    send80cmd(fd,s[i]);
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

int hf2020_process_barrett_line(int serialfd,char *l)
{
  // Skip XON/XOFF character at start of line
  while(l[0]&&l[0]<' ') l++;
  while(l[0]&&(l[strlen(l)-1]<' ')) l[strlen(l)-1]=0;
  
  fprintf(stderr,"Barrett radio says (in state 0x%04x): %s\n",hf_state,l);

  if ((!strcmp(l,"EV00"))&&(hf_state==HF_CALLREQUESTED)) {
    // Syntax error in our request to call.
    printf("Saw EV00 response. Marking call disconnected.\n");
		hf_next_call_time=time(0); //AXLINK failed, no call have been tried
    hf_state = HF_DISCONNECTED;
    return 0;
  }
  if ((!strcmp(l,"E0"))&&(hf_state==HF_CALLREQUESTED)) {
    // Syntax error in our request to call.
    printf("Saw E0 response. Marking call disconnected.\n");
		hf_next_call_time=time(0); //AXLINK failed, no call have been tried
    hf_state = HF_DISCONNECTED;
    return 0;
  }
	if ((!strcmp(l,"EV08"))&&(hf_state==HF_CALLREQUESTED)) {
    // Syntax error in our request to call.
    printf("Saw EV08 response. Marking call disconnected.\n");
		hf_state = HF_DISCONNECTED;
    return 0;
  }

  char tmp[8192];

  if (sscanf(l, "AIATBL%s", tmp)==1){ 

    hf_parse_linkcandidate(l);
    
    //display all the hf radios
    printf("The self hf Barrett radio is: \n%s, index=%s\n", self_hf_station.name, self_hf_station.index);		
    printf("The registered stations are:\n");		
    int i;		
    for (i=0; i<hf_station_count; i++){
      printf("%s, index=%s\n", hf_stations[i].name, hf_stations[i].index);
    }
  }

  if ((!strcmp(l,"AILTBL"))&&((hf_state==HF_ALELINK)||(hf_state==HF_ALESENDING))) {
    if (hf_link_partner>-1) {
	  // Mark link partner as having been attempted now, so that we can
  	// round-robin better.  Basically we should probably mark the station we failed
    // to connect to for re-attempt in a few minutes.
	    hf_stations[hf_link_partner].consecutive_connection_failures++;
	    fprintf(stderr,"Failed to connect to station #%d '%s' (%d times in a row)\n",
		    hf_link_partner,
		    hf_stations[hf_link_partner].name,
		    hf_stations[hf_link_partner].consecutive_connection_failures);
    }
    hf_link_partner=-1;

    // We have to also wait for the > prompt again
    printf("Timed out trying to connect. Marking call disconnected.\n");
    hf_state=HF_DISCONNECTED;
  }
  
  if ((sscanf(l,"AILTBL%s",tmp)==1)&&(hf_state!=HF_ALELINK)&&(hf_state!=HF_DISCONNECTING)&&(hf_state!=HF_ALESENDING)) {

    // Link established
    barrett_link_partner_string[0]=tmp[4];
    barrett_link_partner_string[1]=tmp[5];
    barrett_link_partner_string[2]=tmp[2];
    barrett_link_partner_string[3]=tmp[3];
    barrett_link_partner_string[4]=0;

    int i;
    for(i=0;i<hf_station_count;i++){
      strcpy(tmp, hf_stations[i].index);
      strcat(tmp, self_hf_station.index);
      if (!strcmp(barrett_link_partner_string, tmp)){ 
	hf_link_partner=i;
	hf_stations[hf_link_partner].consecutive_connection_failures=0;
	break; 
      }
    }
    
    if (((hf_state&0xff)!=HF_CONNECTING)
	&&((hf_state&0xff)!=HF_CALLREQUESTED)) {
      // We have a link, but without us asking for it.
      // We leave it connected for a while, to allow the other side to establish a
      // Clover call.
    } else {
      // We requested the call, and now we have it, so try to start the clover call.

      printf("Requesting clover call now that ALE link established.\n");
      
      // Build and send call command
      send80cmd(serialfd,0x10);
      // Always call remote end SERVAL, and set our call ID to be SERVAL on startup,
      // so that Serval calls can be easily identified.
      send80stringz(serialfd,"SERVAL");
      
    }
    
    hf_state=HF_ALELINK;   
  
  }
  
  if ((!strcmp(l,"AIMESS3"))&&(hf_state==HF_CALLREQUESTED)){
    printf("No link established after the call request\n");
    hf_state=HF_DISCONNECTED;
    
  }
  
  if ((hf_state==HF_DISCONNECTING)&&(!strcmp(l,"AILTBL"))){
    hf_link_partner=-1;
  }
  
  return 0;
}

int hf2020_receive_barrett_bytes(int serialfd,unsigned char *bytes,int count)
{
  int i;
  for(i=0;i<count;i++) {
    if (bytes[i]==13||bytes[i]==10) { //end of command detected => if not null, line is processed by lbard
      hf_response_line[hf_rl_len]=0; //	after the command we out a '\0' to have a proper string
      if (hf_rl_len){ hf2020_process_barrett_line(serialfd,hf_response_line);}
      hf_rl_len=0;
    } else {
      if (hf_rl_len<1024) hf_response_line[hf_rl_len++]=bytes[i];
    }
  }
  return 0;
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
	printf("XXX Try to connect to next station.\n");

	// First, we have to tell the Barrett radio to establish an ALE link
	send80cmd(serialfd,0x34);
	char cmd[1024];
	snprintf(cmd,1024,"AXLINK%s%s\r\n", hf_stations[next_station].index, self_hf_station.index);
	write_all(serialfd,cmd,strlen(cmd));
	
	hf_state=HF_CALLREQUESTED;
	
	// Allow enough time for the clover link to be established
	// 1 minute should be enough.
	hf_next_call_time=time(0)+60;
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
    fprintf(stdout,"\nClover 2020 modem changed to state %s\n",hf_state_name(hf_state));
    previous_state=hf_state;
  }
  return 0;
}

extern int last_rx_rssi;
int last_remote_rssi=-1;

int hf2020_parse_reply(unsigned char *m,int len)
{
  if (!len) return -1;

  switch (m[0]) {
  case 0x24:
    // Disconnected
    printf("Marking link disconnected due to 8024 8000\n");
    hf_state=HF_DISCONNECTED;
    break;
  case 0x70:
    // Channel spectra
    break;
  case 0x71:
    // Celcall status
    break;
  case 0x72:
    // Channel statistics
    last_rx_rssi=m[1+0+1];   // local RSSI
    last_remote_rssi=m[1+7+1]; // remote RSSI
    if (last_rx_rssi||last_remote_rssi)
      printf("RSSI = %d local, %d remote (x 0.5 dB)\n",last_rx_rssi,last_remote_rssi);
    break;
  case 0x73:
    // Clover call status
    switch(m[1]) {
    case 0x65:
      printf("Modem reports attempting to establish robust clover call\n");
      break;
    case 0x9C:
      printf("Modem reports clover call failed due to CCB send retries exceeded.\n");

      // Disconnect ALE link
      send80cmd(serialfd,0x34); // output goes to Barrett radio
      write_all(serialfd,"\r\nAXABORT\r\n",11);
      
      hf_state=HF_DISCONNECTED;
      break;
    default:
      printf("Clover call status = 0x%02x\n",m[1]);
      break;
    }
    break;
  default:
    dump_bytes(stdout,"Unknown 80xx status message",m,len);
  }

  return 0;
}

int esc80=0;
int esc91=0;
int fromSecondary=1;
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
    //    printf("saw byte %02x\n",b);    
    if ((!esc91)&&(b==0x91)) {
      esc91=1;
    } else if (esc91) {
      esc91=0;
      if (b==0x33) fromSecondary=1;
      if (b==0x31) fromSecondary=0;
    } else if (esc80) {
      esc80=0;
      //      printf("saw 80 %02x\n",b);
      if (status80BytesRemaining) {
	status80[status80len++]=b;
	status80BytesRemaining--;
	// Check for end of null-terminated string in reply
	if ((status80BytesRemaining<0)&&(b==0x00))
	  {	    
	    status80BytesRemaining=0;
	    hf2020_parse_reply(status80,status80len);
	  }
	// Check for end of fixed-length reply
	if (!status80BytesRemaining) {
	  hf2020_parse_reply(status80,status80len);
	}
      } else {
	switch(b) {
	case 0x06: case 0x09:
	case 0x10:
	case 0x34:
	case 0x51: case 0x56: case 0x59: case 0x5a:
	case 0x65: case 0x69:
	case 0x80:
	case 0xfe:
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
	  printf("Disconnecting due to 80 23 / 80 24 response\n");
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
	case 0x71:
	  // SELcall status
	  status80[0]=b; status80len=1; status80BytesRemaining=1; break;
	case 0x72:
	  // Channel statistics (7 bytes for both ends)
	  status80[0]=b; status80len=1; status80BytesRemaining=7*2; break;
	case 0x73:
	  // Clover call status
	  status80[0]=b; status80len=1; status80BytesRemaining=1; break;
	default:
	  printf("Saw unknown status message: 80 %02x\n",b);
	}
      }
    } else {      
      switch(b) {
      case 0x80: esc80=1; break;
      case 0x91: esc91=1; break;
      default:
	if (fromSecondary) {
	  // Barrett modem output
	  //	  printf("Passing byte 0x%02x to barrett AT command parser\n",b);
	  hf2020_receive_barrett_bytes(serialfd,&b,1);
	} else {
	  // Clover modem output
	  printf("Received HF data byte 0x%02x\n",b);
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
