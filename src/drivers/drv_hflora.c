/*

RADIO TYPE: NORADIO,"noradio","No radio",null_radio_detect,null_serviceloop,null_receive_bytes,null_send_packet,null_check_if_ready,10
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
#include <arpa/inet.h>
#include <netdb.h>

#include "sync.h"
#include "lbard.h"
#include "hf.h"
#include "radios.h"
//#include "code_instrumentation.h"

// Import serial_port string from main.c
extern char *serial_port;

int hflora_radio_detect(int fd)
{
  if ((fd==-1)&&(!strcmp(serial_port,"noradio"))) {
    //LOG_NOTE("No serial port, so no radio");
    fprintf(stderr,"No serial port, so no radio\n");
    radio_set_type(RADIOTYPE_NORADIO);
    return 1;
  }
  else{ 
    unsigned char buf[8192];
    unsigned char loramodule[6];
    unsigned char init[] = "sys get ver\r\n";
    serial_setup_port_with_speed(fd,57600);
    write_all(fd, init, strlen(init)); // ask Codan radio for version
    sleep(1); // give the radio the chance to respond
    ssize_t count = read_nonblock(fd,buf,8192);  // read reply 33
    strncpy(buf,loramodule,6);
    if(hflora_initialise(fd,loramodule)==-1){
      LOG_NOTE("INIT failed !");
      return -1;
    }
    else{
      LOG_NOTE("INIT success !");
      radio_set_type(RADIOTYPE_HFLORA); 
      return 0;
    }
  
  }
}

int hflora_serviceloop(int serialfd)
{
  return 0;
}

int hflora_receive_bytes(unsigned char *bytes,int count)
{ 
  return 0;
}

int hflora_send_packet(int serialfd,unsigned char *out, int len)
{
  return 0;
}

int hflora_check_if_ready(void)
{
  return -1;
}

/*
// LoRa Settings
const RADIO_MODE = "lora";
const RADIO_FREQ = 933000000;
const RADIO_SPREADING_FACTOR = "sf12"; // 128 chips
const RADIO_BANDWIDTH = 125;
const RADIO_CODING_RATE = "4/5";
const RADIO_CRC = "on"; // crc header enabled
const RADIO_SYNC_WORD = 8;
const RADIO_WATCHDOG_TIMEOUT = 0;
const RADIO_POWER_OUT = 14;
const RADIO_RX_WINDOW_SIZE = 0; // contiuous mode

// LoRa Commands
const MAC_PAUSE = "mac pause";
const RADIO_SET = "radio set";
const RADIO_RX = "radio rx";
const RADIO_TX = "radio tx";

// LoRa Com variables
const TX_HEADER = "FF000000";
const TX_FOOTER = "00";
const ACK_COMMAND = "5458204F4B" // "TX OK"
*/
//char lora_link_partner_string[1024]="";
int hflora_initialise(int serialfd, char lora_module)
{
  // See "2050 RS-232 ALE Commands" document from Barrett for more information on the
  // available commands.
  
  // XXX - Issue AXENABT to enable ALE?
  // XXX - Issue AXALRM0 to disable audible alarms when operating? (or 1 to enable them?)
  // XXX - Issue AICTBL to get current ALE channel list?
  // XXX - Issue AIATBL to get current ALE address list
  //       (and use a "serval" prefix on the 0-15 char alias names to auto-pick the parties to talk to?)
  //       (or should it be AINTBL to get ALE network addresses?)
  // XXX - Issue AISTBL to get channel scan list?
  // XXX - Issue ARAMDM1 to register for ALE AMD message notifications?
  // XXX - Issue ARLINK1 to register for ALE LINK notifications?
  // XXX - ARLTBL1 to register for ALE LINK table notifications?
  // XXX - ARMESS1 to register for ALE event notifications?
  // XXX - ARSTAT1 to register for ALE status change notifications?

  int count;
  unsigned char buf[8192];
    
  // Tell Barrett radio we want to know when various events occur.
  switch(lora_module){
  case "RN2903" :
    char *setup_string[7]
      ={
      "radio set mod lora\r\n", // Register for AMD messages
      "radio set freq 933000000\r\n", // Register for phone messages
      "radio set sf sf12\r\n", // Register for new calls
      "radio set bw 125\r\n", // Hear about ALE link notifications
      "radio set cr 4/5\r\n", // Hear about ALE link table events
      "radio set prlen 8\r\n", // Hear about ALE event notifications
      "radio set pwr 14\r\n", // Hear about ALE status change notifications
    };
  break;
  case "RN2483" :
    char *setup_string[7]
      ={
      "radio set mod lora\r\n", // Register for AMD messages
      "radio set freq 433100000\r\n", // Register for phone messages
      "radio set sf sf12\r\n", // Register for new calls
      "radio set bw 125\r\n", // Hear about ALE link notifications
      "radio set cr 4/5\r\n", // Hear about ALE link table events
      "radio set prlen 8\r\n", // Hear about ALE event notifications
      "radio set pwr 14\r\n", // Hear about ALE status change notifications
    };
  break;
  default :
    printf("wrong lora module given\n");
    return(-1);
  break;
  }
  int i;
  for(i=0;i<7;i++) {
    write(serialfd,setup_string[i],strlen(setup_string[i]));
    usleep(200000);
    count = read_nonblock(serialfd,buf,8192);  // read reply
    dump_bytes(stderr,setup_string[i],buf,count);
  }    
  
  return 0;
}
/*
int hflora_serviceloop(int serialfd)
{
  char cmd[1024];
  
  if (!has_hf_plan) {
    fprintf(stderr,"You must specify a HF radio plan via the hfplan= command line option.\n");
    exit(-1);
  }

  switch(hf_state) {
  case HF_DISCONNECTED:
    // Currently disconnected. If the current time is later than the next scheduled
    // call-out time, then pick a hf station to call

    // Wait until we are allowed our first call before doing so
    if (time(0)<last_outbound_call) return 0;
    
    if ((hf_station_count>0)&&(time(0)>=hf_next_call_time)) {
      int next_station = hf_next_station_to_call();
      if (next_station>-1) {
	// Ensure we have a clear line for new command (we were getting some
	// errors here intermittantly).
	write(serialfd,"\r\n",2);
	
	snprintf(cmd,1024,"AXLINK%s\r\n",hf_stations[next_station].name);
	write(serialfd,cmd,strlen(cmd));
	hf_state = HF_CALLREQUESTED;
      
	fprintf(stderr,"HF: Attempting to call station #%d '%s'\n",
		next_station,hf_stations[next_station].name);
      }
    }
    break;
  case HF_CALLREQUESTED:
    // Probe periodically with AILTBL to get link table, because the modem doesn't
    // preemptively tell us when we get a link established
    if (time(0)!=last_link_probe_time)  {
      write(serialfd,"AILTBL\r\n",8);
      last_link_probe_time=time(0);
    }
    break;
  case HF_CONNECTING:
    break;
  case HF_ALELINK:
    // Probe periodically with AILTBL to get link table, because the modem doesn't
    // preemptively tell us when we lose a link
    if (time(0)!=last_link_probe_time)  {
      write(serialfd,"AILTBL\r\n",8);
      last_link_probe_time=time(0);
    }
    break;
  case HF_DISCONNECTING:
    break;
  default:
    break;
  }
  
  return 0;
}

int hflora_process_line(char *l)
{
  // Skip XON/XOFF character at start of line
  while(l[0]&&l[0]<' ') l++;
  while(l[0]&&(l[strlen(l)-1]<' ')) l[strlen(l)-1]=0;
  
  //  fprintf(stderr,"Barrett radio says (in state 0x%04x): %s\n",hf_state,l);

  if ((!strcmp(l,"EV00"))&&(hf_state==HF_CALLREQUESTED)) {
    // Syntax error in our request to call.
    hf_state = HF_DISCONNECTED;
    return 0;
  }
  if ((!strcmp(l,"E0"))&&(hf_state==HF_CALLREQUESTED)) {
    // Syntax error in our request to call.
    hf_state = HF_DISCONNECTED;
    return 0;
  }

  char tmp[8192];

  if (sscanf(l,"AIAMDM%s",tmp)==1) {
    fprintf(stderr,"Barrett radio saw ALE AMD message '%s'\n",&tmp[6]);
    hf_process_fragment(&tmp[6]);
  }
  
  if ((!strcmp(l,"AILTBL"))&&(hf_state==HF_ALELINK)) {
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
      ale_inprogress=0;

      // We have to also wait for the > prompt again
      hf_state=HF_DISCONNECTED;
  } else if ((sscanf(l,"AILTBL%s",tmp)==1)&&(hf_state!=HF_ALELINK)) {
    // Link established
    barrett_link_partner_string[0]=tmp[4];
    barrett_link_partner_string[1]=tmp[5];
    barrett_link_partner_string[2]=tmp[2];
    barrett_link_partner_string[3]=tmp[3];
    barrett_link_partner_string[4]=0;

    int i;
    hf_link_partner=-1;
    for(i=0;i<hf_station_count;i++)
      if (!strcmp(barrett_link_partner_string,hf_stations[i].name))
	{ hf_link_partner=i;
	  hf_stations[hf_link_partner].consecutive_connection_failures=0;
	  break; }

    if (((hf_state&0xff)!=HF_CONNECTING)
	&&((hf_state&0xff)!=HF_CALLREQUESTED)) {
      // We have a link, but without us asking for it.
      // So allow 10 seconds before trying to TX, else we start TXing immediately.
    hf_radio_pause_for_turnaround();
    } else hf_radio_mark_ready();

    
    fprintf(stderr,"ALE Link established with %s (station #%d), I will send a packet in %ld seconds\n",
	    barrett_link_partner_string,hf_link_partner,
	    hf_next_packet_time-time(0));
    
    hf_state=HF_ALELINK;
  }

  return 0;
}

int hflora_receive_bytes(unsigned char *bytes,int count)
{
  int i;
  for(i=0;i<count;i++) {
    if (bytes[i]==13||bytes[i]==10) {
      hf_response_line[hf_rl_len]=0;
      if (hf_rl_len) hfbarrett_process_line(hf_response_line);
      hf_rl_len=0;
    } else {
      if (hf_rl_len<1024) hf_response_line[hf_rl_len++]=bytes[i];
    }
  }
  return 0;
}

int hfbarrett_send_packet(int serialfd,unsigned char *out, int len)
{
  // We can send upto 90 ALE encoded bytes.  ALE bytes are 6-bit, so we can send
  // 22 groups of 3 bytes = 66 bytes raw and 88 encoded bytes.  We can use the first
  // two bytes for fragmentation, since we would still like to support 256-byte
  // messages.  This means we need upto 4 pieces for each message.
  char message[8192];
  char fragment[8192];

  int i;

  time_t absolute_timeout=time(0)+90;

  if (hf_state!=HF_ALELINK) return -1;
  if (ale_inprogress) return -1;
  if (!barrett_link_partner_string[0]) return -1;
  
  // How many pieces to send (1-6)
  // This means we have 36 possible fragment indications, if we wish to imply the
  // number of fragments in the fragment counter.
  int pieces=len/43; if (len%43) pieces++;
  
  fprintf(stderr,"Sending message of %d bytes via Barratt HF\n",len);
  for(i=0;i<len;i+=43) {
    // Indicate radio type in fragment header
    fragment[0]=0x41+(hf_message_sequence_number&0x07);
    fragment[1]=0x30+(i/43);
    fragment[2]=0x30+pieces;
    int frag_len=43; if (len-i<43) frag_len=len-i;
    hex_encode(&out[i],&fragment[3],frag_len,radio_get_type());

    unsigned char buffer[8192];
    int count;

    usleep(100000);
    count = read_nonblock(serialfd,buffer,8192);
    if (count) dump_bytes(stderr,"presend",buffer,count);
    if (count) hfbarrett_receive_bytes(buffer,count);
    
    snprintf(message,8192,"AXNMSG%s%02d%s\r\n",
	     barrett_link_partner_string,
	     (int)strlen(fragment),fragment);

    int not_accepted=1;
    while (not_accepted) {
      if (time(0)>absolute_timeout) {
	fprintf(stderr,"Failed to send packet in reasonable amount of time. Aborting.\n");
	hf_message_sequence_number++;
	return -1;
      }
      
      write_all(serialfd,message,strlen(message));

      // Any ALE send will take at least a second, so we can safely wait that long
      sleep(1);

      // Check that it gets accepted for TX. If we see EV04, then something is still
      // being sent, and we have to wait and try again.
      count = read_nonblock(serialfd,buffer,8192);
      // if (count) dump_bytes(stderr,"postsend",buffer,count);
      if (count) hfbarrett_receive_bytes(buffer,count);
      if (strstr((const char *)buffer,"OK")
	  &&(!strstr((const char *)buffer,"EV"))) {
	not_accepted=0;
	char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
	if (timestr[0]) timestr[strlen(timestr)-1]=0;
	fprintf(stderr,"  [%s] Sent %s",timestr,message);

      } else not_accepted=1;
      
    }    

  }
  hf_radio_pause_for_turnaround();
  hf_message_sequence_number++;
  char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
  if (timestr[0]) timestr[strlen(timestr)-1]=0;
  fprintf(stderr,"  [%s] Finished sending packet, next in %ld seconds.\n",
	  timestr,hf_next_packet_time-time(0));
  
  return 0;
}*/