
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

#define CLOVER_ID "serval"

int hf2020_received_byte(unsigned char b);

extern int serialfd;
extern char barrett_link_partner_string[1024];
extern int previous_state;

int hf_may_defer=-1;
time_t hf_connecting_timeout=0;
time_t clover_connect_time=0;
int clover_tx_buffer_space=1024;

int send80cmd(int fd,unsigned char c)
{
  unsigned char cmd[2];
  cmd[0]=0x80; cmd[1]=c;
  write_all(fd,cmd,2);

  //   printf("80 %02x written.\n",c);
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
  // XXX  if (ale_inprogress) isReady=0;
  // XXX  if (!barrett_link_partner_string[0]) isReady=0;

  return isReady; 
}

int hf2020_initialise(int fd, unsigned int product_id)
{
  fprintf(stderr,"Detected a Clover HF modem, model ID %04x\n",
	  product_id);

  hf_may_defer=1;
  
  // Reset takes a long time, so don't do anything now.
  // send80cmd(fd,0x09); // Hardware reset
  
  send80cmd(fd,0x06); // Stop whatever we are doing
  send80cmd(fd,0x80); // Switch to clover mode
  send80cmd(fd,0x65); send80cmd(fd,0x02); // Set FEC mode to "FAST"
  send80cmd(fd,0x59); // Enable echo-as-sent
  send80cmd(fd,0xfe); send80cmd(fd,0x00); send80cmd(fd,0x00); // use default CRC mask
  // send80cmd(fd,0x51); // enable channel status reports
  send80cmd(fd,0x41); // disable channel status reports
  send80cmd(fd,0x52); // answer incoming calls automatically
  send80cmd(fd,0x56); // enable expanded link state reports
  send80cmd(fd,0x5a); // enable even more reports

  // We set our call ID to always be the same, SERVAL, so that Serval clover calls
  // can be easily recognised.
  send80cmd(fd,0x13); send80stringz(fd,CLOVER_ID);
  
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

  
  // Now do Barrett radio setup
  // Tell Barrett radio we want to know when various events occur.
  char *setup_string[11]
    ={
		"AIATBL\r\n", // Ask for all valid ale addresses
		"ARAMDM1\r\n", // Register for AMD messages
		"ARAMDP1\r\n", // Register for phone messages
		"ARCALL1\r\n", // Register for new calls
		"ARLINK1\r\n", // Hear about ALE link notifications
		"ARLTBL1\r\n", // Hear about ALE link table events
		"ARMESS1\r\n", // Hear about ALE event notifications
		"ARSTAT1\r\n", // Hear about ALE status change notifications
		"AXALRM0\r\n", // Diable alarm sound when receiving a call
		"AILTBL\r\n", // Ask for the current ALE link state
		"AXSCNPF\r\n", // Resume channel scanning, if it was paused
  };
  int i;
  write(serialfd,"\r\n",2);
  for(i=0; i<11; i++) {
    write(serialfd,setup_string[i],strlen(setup_string[i]));
    usleep(200000);
  }    
  
  return 1;
}

int hf2020_process_barrett_line(int serialfd,char *l)
{
  // Skip XON/XOFF character at start of line
  while(l[0]&&l[0]<' ') l++;
  while(l[0]&&(l[strlen(l)-1]<' ')) l[strlen(l)-1]=0;

  if (1) if (hf_state!=HF_DISCONNECTED)
    fprintf(stderr,"Barrett radio says (in state %s): %s\n",hf_state_name(hf_state),l);

  if ((!strcmp(l,"EV00"))&&(hf_state==HF_CALLREQUESTED)) {
    // Syntax error in our request to call.
    printf("Saw EV00 response. Marking call disconnected.\n");
    hf_next_call_time=time(0); //AXLINK failed, no call have been tried
    hf_may_defer=1;
    hf_link_partner=-1;
    hf_state = HF_DISCONNECTED;
    return 0;
  }
  if ((!strcmp(l,"E0"))&&(hf_state==HF_CALLREQUESTED)) {
    // Syntax error in our request to call.
    printf("Saw E0 response. Marking call disconnected.\n");
    hf_next_call_time=time(0); //AXLINK failed, no call have been tried
    hf_may_defer=1;
    hf_link_partner=-1;
    hf_state = HF_DISCONNECTED;
    return 0;
  }
  if ((!strcmp(l,"EV08"))&&(hf_state==HF_CALLREQUESTED)) {
    // Syntax error in our request to call.
    printf("Saw EV08 response. Marking call disconnected.\n");
    hf_link_partner=-1;
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

  if (!strcmp(l,"AILTBL")) {
    if (hf_link_partner>-1) {
	  // Mark link partner as having been attempted now, so that we can
  	// round-robin better.  Basically we should probably mark the station we failed
    // to connect to for re-attempt in a few minutes.
	    hf_stations[hf_link_partner].consecutive_connection_failures++;
	    fprintf(stderr,"Disconnected or failed to connect to station #%d '%s' (%d times in a row)\n",
		    hf_link_partner,
		    hf_stations[hf_link_partner].name,
		    hf_stations[hf_link_partner].consecutive_connection_failures);
    }
    hf_link_partner=-1;

    // We have to also wait for the > prompt again
    if (hf_state==HF_CONNECTING||hf_state==HF_CALLREQUESTED) {
      printf("Timed out trying to connect. Marking call disconnected.\n");
      hf_link_partner=-1;
      hf_state=HF_DISCONNECTED;
    }
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
	printf("Setting hf_link_partner=%d\n",hf_link_partner);
	if (hf_state==HF_DISCONNECTED) {
	  // Er, okay. So we thought we were disconnected, but have an ALE link established.
	  // So mark us as connecting, so we can time out, and don't try to call anyone else just yet.

	  hf_state=HF_CONNECTING;
	  hf_connecting_timeout=time(0)+30+(random()%30);
	}
	break; 
      }
    }
    
    if ((hf_state&0xff)!=HF_CALLREQUESTED) {
      // We have a link, but without us asking for it.
      // We leave it connected for a while, to allow the other side to establish a
      // Clover call.
      hf_state=HF_CONNECTING;
      hf_connecting_timeout=time(0)+30+(random()%30);
    } 

    if ((hf_link_partner!=-1)&&(!clover_connect_time)) {
      
      printf("Requesting clover call now that ALE link established.\n");

      clover_connect_time=time(0)+random()%10;
      
    }    
  }
  
  if ((!strcmp(l,"AIMESS3"))&&(hf_state==HF_CALLREQUESTED)){
    printf("No link established after the call request -- resuming scanning\n");
    hf_link_partner=-1;
    hf_state=HF_DISCONNECTED;

    // Resume scanning
    send80cmd(serialfd,0x34); // output goes to Barrett radio
    write_all(serialfd,"XN1\r\n",10);
    
  }

  if ((!strcmp(l,"AIMESS2"))&&(hf_state==HF_CALLREQUESTED)){
    printf("Call disconnected -- resuming scanning.\n");
    hf_link_partner=-1;
    hf_state=HF_DISCONNECTED;

    // Resume channel scanning
    send80cmd(serialfd,0x34); // output goes to Barrett radio
    write_all(serialfd,"XN1\r\n",10);
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
      hf_link_partner=-1;
    }
    
    // Wait until we are allowed our first call before doing so
    if (time(0)<last_outbound_call)
      {
	printf("Not yet allowed to call out.\n");
	return 0;
      }
    
    // Currently disconnected. If the current time is later than the next scheduled
    // If the radio is not receiving a message
    // call-out time, then pick a hf station to call

    if ((hf_link_partner==-1)&&(hf_station_count>0)&&(time(0)>=hf_next_call_time)) {
      printf("It would be good to call another station.\n");
      int next_station = hf_next_station_to_call();
      if (next_station>-1) {
	// XXX Try to connect
	printf(">>> Try to connect to station '%s'.\n",hf_stations[next_station].name);

	// First, we have to tell the Barrett radio to establish an ALE link
	send80cmd(serialfd,0x34);
	char cmd[1024];
	snprintf(cmd,1024,"AXLINK%s%s\r\n", hf_stations[next_station].index, self_hf_station.index);
	write_all(serialfd,cmd,strlen(cmd));
	
	hf_state=HF_CALLREQUESTED;
	
	// Allow enough time for the clover link to be established
	// 1 minute should be enough.
	// (add randomness to prevent lock-step)
	hf_next_call_time=time(0)+30+(random()%30);
	hf_may_defer=1;
      }
    }
    else if (hf_link_partner>-1) {
      // If we are connected to someone, then mark us as being in-call
      // XXX This probably needs to change for the 2020
      // hf_state=HF_ALELINK;
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
    if (time(0)>=hf_next_call_time){ //no reply from the called station
      hf_state = HF_DISCONNECTED;
      hf_link_partner=-1;
      printf("Make the call disconnected because of no reply\n");
    }
    if (clover_connect_time&&(time(0)>=clover_connect_time))
      {
	printf("Sending command to establish clover link now.\n");
	clover_connect_time=0;
#if 0
	// Was used to test that we receive the clover packets.
	send80cmd(serialfd,0x14);
	send80stringz(serialfd,"Hello world!");
#else
	// Build and send call command
	send80cmd(serialfd,0x10);
	// Always call remote end SERVAL, and set our call ID to be SERVAL on startup,
	// so that Serval calls can be easily identified.
	send80stringz(serialfd,CLOVER_ID);
#endif
      }
    
    break;
    
  case HF_CONNECTING: //3
    if (time(0)>hf_connecting_timeout) {
      hf_state=HF_DISCONNECTED;
      hf_link_partner=-1;
    }
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

    if (clover_tx_buffer_space>512) {
      // Space in the TX buffer, so push a new packet.
      unsigned char msg_out[256];
      update_my_message(
			serialfd,
			my_sid,
			my_sid_hex,
			255,
			msg_out,
			servald_server,
			credential);
    }
    
    break;

  case HF_DISCONNECTING: //5
    
    printf("XXX Aborting the current established ALE link\n");

    hf_link_partner=-1;
    hf_state=HF_DISCONNECTED;
    
    break;

  case HF_ALESENDING: //6
    // This state is managed in the hf2020_send_packet function
    break;

  case HF_RADIOCONFUSED: //7
    // We entenred a case where the radio is confused
    printf("XXX HF Radio is confused. Marking as disconnected.\n");
    hf_link_partner=-1;
    hf_state=HF_DISCONNECTED;
    
    break;

  default:
		
    break;
  }

  if (previous_state!=hf_state){
    fprintf(stdout,"\nClover 2020 modem changed to state %s (next call in %lld seconds)\n",
	    hf_state_name(hf_state),(long long)(hf_next_call_time-time(0)));
    previous_state=hf_state;
  } else {
    static int last_state_report_time=0;    
    if (time(0)>last_state_report_time) {
      fprintf(stdout,"Link state is %s (next call in %lld seconds, HF_CONNECTING timeout = %lld, link partner=%d, hf_station_count=%d)\n",
	      hf_state_name(hf_state),
	      (long long)(hf_next_call_time-time(0)),
	      (long long)(hf_connecting_timeout-time(0)),
	      hf_link_partner,hf_station_count);
      last_state_report_time=time(0)+10;
    }
  }
  
  return 0;
}

extern int last_rx_rssi;
int last_remote_rssi=-1;

int hf2020_parse_reply(unsigned char *m,int len)
{
  if (!len) return -1;

  switch (m[0]) {
  case 0x20:
    printf("Linked via clover to '%s'\n",&m[1]);
    // XXX Check if link partner is "serval". If not, disconnect.
    hf_state=HF_ALELINK;
    break;
  case 0x22:
    printf("Monitored ARQ from '%s'\n",&m[1]);
  case 0x24:
    // Disconnected
    printf("Marking link disconnected due to 8024 8000\n");
    hf_link_partner=-1;
    hf_state=HF_DISCONNECTED;
    break;
  case 0x27:
    // Incoming clover call to us
    printf("Incoming clover call addressed to us.\n");
    break;
  case 0x28:
    printf("Monitored ARQ to '%s' (whatever that means)\n",&m[1]);
    break;
  case 0x42:
    printf("Link stations monitored for clover (whatever that means)\n");
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
  case 0x65:
      printf("Modem reports attempting to establish robust clover call\n");
      break;
  case 0x73:
    // Clover call status
    switch(m[1]) {
    case 0x01:
      // Here we should either just give up, or try again soon.
      // For now, we will give up.
      printf("Modem reports channel occupied by non-clover signal (that could just be ALE links establishing).\n");
      if (hf_state==HF_DISCONNECTED) {
	// If not yet connected, don't try to open a call just yet, if the channel is busy
	// (But at the same time, we don't want scanning past a busy channel to stop us communicating)
	// So extend the wait exactly one time only.
	if (hf_may_defer) {
	  printf("Deferring trying to make a call, incase this is an incoming call for us.\n");
	  hf_next_call_time=time(0)+20+(random()%20);
	  hf_may_defer=0;
	}
      }
      break;
    case 0x65: printf("Clover modem attempting robust link\n"); break;
    case 0x78: printf("Send clover call CCB RETRY\n"); break;
    case 0x79: printf("Receiving clover call CCB RETRY\n"); break;
    case 0x7D:
      printf("Received clover call CCB OK\n");
      // Sometimes ALE thinks we have hung up, when the clover link is still running,
      // so reinstate the link if we keep seeing CCB traffic.
      if (hf_state==HF_DISCONNECTED) hf_state=HF_ALELINK;
      break;
    case 0x7f: printf("!!! Clover modem reports an error with a recent command\n"); break;
    case 0x8e:
      // TX is idle, so assume buffer is empty.
      printf("TX idle, marking Clover TX buffer empty.\n");
      clover_tx_buffer_space=1024;
      break;
    case 0x9C:
      printf("Modem reports clover call failed due to CCB send retries exceeded.\n");

      // Disconnect ALE link
      send80cmd(serialfd,0x34); // output goes to Barrett radio
      //      write_all(serialfd,"\r\nAXABORT\r\n",11);
      write_all(serialfd,"AXTLNK00\r\n",10);
      
      hf_link_partner=-1;
      hf_state=HF_DISCONNECTED;
      break;
    default:
      printf("Clover call status = 0x%02x\n",m[1]);
      break;
    }
    break;
  case 0x75:
    printf("Clover connection waveform format: %02x %02x %02x %02x\n",
	   m[1],m[2],m[3],m[4]);
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
    //    printf("saw byte %02x '%c'\n",b, ((b==' ')||(b>='0'&&b<=0x7d)?b:'X'));    
    if ((!esc91)&&(b==0x91)) {
      esc91=1;
    } else if (esc91) {
      esc91=0;
      switch(b) {
      case 0x33: fromSecondary=1; break;
      case 0x31: fromSecondary=0; break;
      case 0xb1: 
	b=0x91;
	if (fromSecondary) {
	  // Barrett modem output
	  //	  printf("Passing byte 0x%02x to barrett AT command parser\n",b);
	  hf2020_receive_barrett_bytes(serialfd,&b,1);
	} else {
	  // Clover modem output
	  //	  printf("Received HF data byte 0x%02x\n",b);
	  hf2020_received_byte(b);
	}
	break;
      }
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
	case 0x33: case 0x34:
	case 0x51: case 0x56: case 0x59: case 0x5a:
	case 0x65: case 0x69:
	case 0x80:
	case 0xfe:
	  // Various status messages we can safely ignore
	  break;
	case 0x20: case 0x21: case 0x22:
	case 0x26: case 0x27: case 0x28: case 0x2a:
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

	  // Disconnect ALE link
	  send80cmd(serialfd,0x34); // output goes to Barrett radio
	  //      write_all(serialfd,"\r\nAXABORT\r\n",11);
	  write_all(serialfd,"AXTLNK00\r\n",10);
	  
	  hf_link_partner=-1;
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
	case 0x75:
	  // Clover call waveform format
	  status80[0]=b; status80len=1; status80BytesRemaining=4; break;
	case 0x7f:
	  // Modem command error
	  status80[0]=b; status80len=1; status80BytesRemaining=2; break;
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
	  //	  printf("Received HF data byte 0x%02x\n",b);
	  hf2020_received_byte(b);
	}
      }
    }
  }
  return 0;
}

unsigned char hf2020_rx_buffer[512];

int hf2020_received_byte(unsigned char b)
{
  // Add new byte to end of the buffer
  
  // A ring buffer would be more efficient, but at 9600bps
  // this will be fine.
  bcopy(&hf2020_rx_buffer[1],&hf2020_rx_buffer[0],512-1);
  hf2020_rx_buffer[511]=b;

  // Check for envelope trailer
  if ((hf2020_rx_buffer[511-6]==0x55)&&
      (hf2020_rx_buffer[511-5]==0xAA)&&
      (hf2020_rx_buffer[511-4]==0x55)&&
      ((hf2020_rx_buffer[511-3]&0xf0)==0)&&
      ((hf2020_rx_buffer[511-2]&0xf0)==0)&&
      (hf2020_rx_buffer[511-1]==0xAA)&&
      (hf2020_rx_buffer[511-0]==0x55)) {

    // Okay, so envelope trailer is present.
    // Work out the length of the packet we expect to see
    int candidate_len=hf2020_rx_buffer[511-2]<<4;
    candidate_len+=hf2020_rx_buffer[511-3];

    printf("Envelope for a %d byte packet found\n",candidate_len);
    
    // Now see if the start of packet marker preceeds the
    // expected packet
    if ((hf2020_rx_buffer[511-7-candidate_len-1]==0x91)&&
	(hf2020_rx_buffer[511-7-candidate_len-0]==0x90)) {
      printf("We have found a packet of %d bytes\n",
	     candidate_len);
      dump_bytes(stdout,"The packet:",&hf2020_rx_buffer[511-6-candidate_len],candidate_len);
      saw_packet(&hf2020_rx_buffer[511-6-candidate_len],candidate_len,
		 last_rx_rssi,
		 my_sid_hex,prefix,servald_server,credential);
    }
  }
  
  return 0;
}

int hf2020_send_packet(int serialfd,unsigned char *out, int len)
{
  printf("Preparing to send a packet of %d bytes\n",len);
  
  // Now send the packet with escaping of the appropriate bytes.
  // But first, redirect output to modem TX

  // Switch to TX via modem
  send80cmd(serialfd,0x33);

  unsigned char escaped[256+256*2];
  int elen=0;

  // Add marker bytes to beginning as part of the envelope
  escaped[elen++]=0x91;
  escaped[elen++]=0x90;
  
  // Write packet body with escape characters
  for(int i=0;i<len;i++) {
    switch(out[i]) {
    case 0x80: case 0x81:
      escaped[elen++]=0x81; escaped[elen++]=out[i];
      clover_tx_buffer_space--;
      break;
    case 0x90: case 0x91:
      escaped[elen++]=0x91; escaped[elen++]=out[i];
      clover_tx_buffer_space--;
      break;
    default:
      escaped[elen++]=out[i];
      clover_tx_buffer_space--;
      break;
    }
    if (clover_tx_buffer_space<0) clover_tx_buffer_space=0;
  }

  // Add evenlope to the end, so that we know when we have a packet
  // (The odd false positive from matching part of the envelope with
  // data should not cause any significant problems. Later we can
  // implement yet another escape character to make it foolproof.)
  escaped[elen++]=0x55;
  escaped[elen++]=0xAA;
  escaped[elen++]=0x55;
  // Encode the length as two nybls, so that we can be sure to
  // not trigger an escape character.
  escaped[elen++]=len&0xf;
  escaped[elen++]=(len>>4)&0xf;
  escaped[elen++]=0xAA;
  escaped[elen++]=0x55;
  clover_tx_buffer_space-=7;
    
  //  dump_bytes(stdout,"Escaped packet for Clover TX",escaped,len);
  write_all(serialfd,escaped,elen);

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
  usleep(700000);
  int count=read_nonblock(fd,buffer,1024);
  
  unsigned int product_id=hf2020_extract_product_id(buffer,count);

  if (!product_id) {
    serial_setup_port_with_speed(fd,115200);
    send80cmd(fd,0x7B);
    usleep(700000);
    count=read_nonblock(fd,buffer,1024);
    product_id=hf2020_extract_product_id(buffer,count);
  }

  if (product_id) {
    radio_set_type(RADIOTYPE_HF2020);
    return hf2020_initialise(fd,product_id);    
  }

  return -1;
}
