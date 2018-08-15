
/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: HF2020,"hf2020","Clover 2020 HF modem",hf2020_radio_detect,hf2020_serviceloop,hf2020_receive_bytes,hf2020_send_packet,hf2020_my_turn_to_send,20

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

int hf2020_initialise(int serialfd, unsigned int product_id)
{
  fprintf(stderr,"Detected a Clover HF modem, model ID %04x\n",
	  product_id);
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

int hf2020_receive_bytes(unsigned char *bytes,int count)
{
  int i;
  for(i=0;i<count;i++) {
    
  }
  return 0;
}

int hf2020_send_packet(int serialfd,unsigned char *out, int len)
{
  return 0;
}

int send80cmd(int fd,unsigned char c)
{
  unsigned char cmd[2];
  cmd[0]=0x80; cmd[1]=c;
  write_all(fd,cmd,2);

  printf("0x80%02x written. Waiting for reply.\n",c);
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
