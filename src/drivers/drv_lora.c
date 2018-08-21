/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: ALORA,"rfdlora","RFD Tri-Band LoRa module",rfdlora_radio_detect,rfdlora_serviceloop,rfdlora_receive_bytes,rfdlora_send_packet,rfdlora_check_if_ready,20
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

int rfdlora_initialise(int fd, int lora_module); 
int rfdlora_switch_module(int fd, int lora_module);
int rfdlora_module_reset(int fd);
int rfdlora_module_ver(int fd);
int rfdlora_module_firmware(int fd, char* firmware);
int rfdlora_module_snr(int fd, char * snr);
void rfdlora_send_packets(int fd);
void rfdlora_receive_packets(int fd);
// Import serial_port string from main.c
extern char *serial_port;
extern int serialfd;
//int last_rx_rssi=-1;
int rfdlora_radio_detect(int fd)
{
  if (fd==-1){
    //LOG_NOTE("No serial port, so no radio");
    fprintf(stderr,"No serial port, so no radio\n");
    return 0;
  }
  else{ 
    //FILE* f=fopen("loralogs.log","w");
    serial_setup_port_with_speed(fd,57600);
    int lora_value = rfdlora_module_reset(fd); //reset the lora radio we are communicating with and retrieve module identifier (RN2903 or RN4843)
    if(rfdlora_initialise(fd,lora_value)==-1){ //set lora radio parameters
      fprintf(stderr,"init failed\n");
      return 0;
    }
    else{
      fprintf(stderr,"RN2903 initialized\n");
      fprintf(stderr,"switching to other module\n");
      //radio_set_type(RADIOTYPE_ALORA); 
      rfdlora_switch_module(fd, lora_value); //switch to the other lora radio module 
      int lora_value = rfdlora_module_reset(fd); //reset the lora radio module
      if(rfdlora_initialise(fd,lora_value)==-1){ //set lora radio parameters
        fprintf(stderr,"init failed\n");
        return 0;
      }
      else{
        fprintf(stderr,"RN4843 initialized\n");
      }
      int version = rfdlora_module_ver(fd); //get lora module name (RN2903 or RN4843)
      char firmware[1024] = {0};
      rfdlora_module_firmware(fd, firmware); //get lora module firmware version
      fprintf(stderr,"module version : %d  -- 0 = RN2903 and 1 = RN2483\n",version);
      fprintf(stderr,"module firmware : %s\n", firmware);
      radio_set_type(RADIOTYPE_ALORA);
      return 1;
    }
  
  }
}

int rfdlora_serviceloop(int fd)
{
  // Deal with clocks running backwards sometimes
  if ((congestion_update_time-gettime_ms())>4000)
    congestion_update_time=gettime_ms()+4000;
  
  if (gettime_ms()>congestion_update_time) {
    /* Very 4 seconds count how many radio packets we have seen, so that we can
       dynamically adjust our packet rate based on our best estimate of the channel
       utilisation.  In other words, if there are only two devices on channel, we
       should be able to send packets very often. But if there are lots of stations
       on channel, then we should back-off.
    */

    double ratio = 1.00;
    if (target_transmissions_per_4seconds)
      ratio = (radio_transmissions_seen+radio_transmissions_byus)
	*1.0/target_transmissions_per_4seconds;
    else {
      fprintf(stderr,"WARNING: target_transmissions_per_4seconds = 0\n");
    }
    // printf("--- Congestion ratio = %.3f\n",ratio);
    if (ratio<0.95) {
      // Speed up: If we are way too slow, then double our rate
      // If not too slow, then just trim 10ms from our interval
      if (ratio<0.25) message_update_interval/=2;
      else {
	int adjust=10;
	if ((ratio<0.80)&&(message_update_interval>300)) adjust=20;
	if ((ratio<0.50)&&(message_update_interval>300)) adjust=50;
	if (ratio>0.90) adjust=3;
	// Only increase our packet rate, if we are not already hogging the channel
	// i.e., we are allowed to send at most 1/n of the packets.
	float max_packets_per_second=1;
	int active_peers=active_peer_count();
	if (active_peers) {
	  max_packets_per_second=(TARGET_TRANSMISSIONS_PER_4SECONDS/active_peers)
	    /4.0;
	}
	int minimum_interval=1000.0/max_packets_per_second;
	if (radio_transmissions_byus<=radio_transmissions_seen)
	  message_update_interval-=adjust;
	if (message_update_interval<minimum_interval)
	  message_update_interval=minimum_interval;
      }
    } else if (ratio>1.0) {
      // Slow down!  We slow down quickly, so as to try to avoid causing
      // too many colissions.
      message_update_interval*=(ratio+0.4);
      if (!message_update_interval) message_update_interval=50;
      if (message_update_interval>4000) message_update_interval=4000;
    }
    
    if (!radio_transmissions_seen) {
      // If we haven't seen anyone else transmit anything, then only transmit
      // at a slow rate, so that we don't jam the channel and flatten our battery
      // while waiting for a peer
      message_update_interval=1000;
    }
    
    // Make randomness 1/4 of interval, or 25ms, whichever is greater.
    // The addition of the randomness means that we should never actually reach
    // our target capacity.
    message_update_interval_randomness = message_update_interval >> 2;
    if (message_update_interval_randomness<25)
      message_update_interval_randomness=25;
    
    // Force message interval to be at least 150ms + randomness
    // This keeps duty cycle < about 10% always.
    // 4 - 5 packets per second is therefore the fastest that we will go
    // (256 byte packet @ 128kbit/sec takes ~20ms)
    if (message_update_interval<150)
      message_update_interval=150;
    
    printf("*** TXing every %d+1d%dms, ratio=%.3f (%d+%d)\n",
	   message_update_interval,message_update_interval_randomness,ratio,
	   radio_transmissions_seen,radio_transmissions_byus);
    congestion_update_time=gettime_ms()+4000;
    
    if (radio_transmissions_seen) {
      radio_silence_count=0;
    } else {
      radio_silence_count++;
      if (radio_silence_count>3) {
	// Radio silence for 4x4sec = 16 sec.
	// This might be due to a bug with the UHF radios where they just stop
	// receiving packets from other radios. Or it could just be that there is
	// no one to talk to. Anyway, resetting the radio is cheap, and fast, so
	// it is best to play it safe and just reset the radio.
	write_all(fd,"!Z",2);
	radio_silence_count=0;
      }
    }
    
    radio_transmissions_seen=0;
    radio_transmissions_byus=0;
  }
  
  return 0;
}

int rfdlora_parse_line(char *line)
{
  if (!strncmp("radio_rx ",(char *)line,9)) {
    char buf[8192];
    char rssi[1024] = {0};
    rfdlora_module_snr(serialfd, rssi);
    // We have a packet from the radio.
    // Decode the hex bytes into an array, and then call saw_packet() with the result
    int i=0;
    do{
        buf[i]=line[i+9];
        i++;
        
    } while(buf[i]);
    
    size_t len = i;
    // Fail if we see an odd number of hex digits, as that means it isn't a whole number
    // of bytes
    if(len&1){
      return -1;
    }
    size_t final_len = len/2;
    unsigned char chrs[final_len+1];
    // Convert hex string to byte array
    for (size_t i=0, j=0; j<final_len; i+=2, j++){
      // Get the value of each pair of hex digits
      chrs[j] = chartohex(line[i])<<4;
      chrs[j]+= chartohex(line[i+1]);     
    }
    chrs[final_len] = 0;
    
    saw_packet(chrs,final_len,atoi(rssi), my_sid_hex,prefix, servald_server,credential);
  }
  return 0;
}

#define MAX_RX_BYTES 8192
char rfdlora_current_rx_line[MAX_RX_BYTES];
int rfdlora_current_rx_line_length=0;
int rfdlora_receive_bytes(unsigned char *bytes,int count)
{ 
  for(int i=0;i<count;i++)
  {
      switch(bytes[i]) {
        case '\n': case '\r':
          rfdlora_parse_line(rfdlora_current_rx_line);
          rfdlora_current_rx_line_length=0;
          break;
        default:
          if (rfdlora_current_rx_line_length<MAX_RX_BYTES) {
            rfdlora_current_rx_line[rfdlora_current_rx_line_length++]=bytes[i];
            rfdlora_current_rx_line[rfdlora_current_rx_line_length]=0;
          }
      }
  }

  return 0;
}

int rfdlora_send_packet(int fd,unsigned char *out, int len)
{
  char macp[] = "mac pause\r\n";
  char radio_resume[] = "radio rx 0\r\n";

  // Reject if the packet is too large
  if (len>255) return -1;
  
  // Convert packet to hex
  char packet_as_hex[1024];
  for(int i=0;i<len;i++) {
    // Convert each byte to a pair of hex characters
    packet_as_hex[i*2+0]=hextochar(out[i]>>4);
    packet_as_hex[i*2+1]=hextochar(out[i]&0x0f);
  }
  // Null terminate hex formatted packet
  packet_as_hex[len*2]=0;
  
  // Build command to TX packet
  char tx[1024]; 
  snprintf(tx,1024,"radio tx %s\r\n",packet_as_hex);

  // set the mac layer in pause in order to prepare for packet send
  write_all(fd, macp, strlen(macp));

  // ask Lora radio to send the packet
  write_all(fd, tx, strlen(tx)); 
  
  // Turn radio RX mode back on
  write_all(fd, radio_resume,strlen(radio_resume));

  return 0;
}

int rfdlora_check_if_ready(void)
{
  return -1;
}

int rfdlora_switch_module(int fd, int lora_module){
  if (lora_module==0){
    char switchm[] = "sys set pindig GPIO13 1\r\n";
    write_all(fd, switchm, strlen(switchm)); // change value of GPIO pin 13 = switch the radio we are communicating with
    //fprintf(stderr,"\n\n-----------------------------------------------------------------------\n\n |%s| \n\n-----------------------------------------------------------------------\n\n",buf);
    return 0;
  }else if (lora_module==1){
    char switchm[] = "sys set pindig GPIO13 0\r\n";
    write_all(fd, switchm, strlen(switchm)); // ask Lora radio for module and version strlen(init)
    //fprintf(stderr,"\n\n-----------------------------------------------------------------------\n\n |%s| \n\n-----------------------------------------------------------------------\n\n",buf);
    return 0;
  }else{
    fprintf(stderr,"Wrong loramodule value");
    return -1;
  }
  //return 0;
}

int rfdlora_module_reset(int fd){
    unsigned char buf[8192];
    char loramodule[7];
    for(int i=0;i<7;i++){
      loramodule[i]='\0';
    }
    //unsigned clr[3]={21,13,10};
    char reset[] = "sys reset\r\n";
    //write_all(fd,clr,3); // Clear any partial command
    //sleep(1);
    //ssize_t count = read_nonblock(fd,buf,8192);  // read and ignore any stuff
    write_all(fd,"\r\n",2); // Clear any partial command
    usleep(100000);
    int count=read_nonblock(fd,buf,8192);  // read and ignore any stuff
    dump_bytes(stdout,"bytes following CRLF",buf,count);
    write_all(fd, reset, strlen(reset)); // reset Lora radio 
    int lora_value=3;

    //not working with smaller value of usleep here, the module did not have the time to reset and therefore didn't send any response in time
    usleep(1000000); 
    int count=read_nonblock(fd,buf,8192);
    dump_bytes(stdout,"bytes following reset",buf,count);

    if (count<7){ printf("%d\n",count);return -1;}
    
    //update the loramodule to know which module we are communicating with then return it
    for(int i=0;i<6;i++){
      loramodule[i]=buf[i];
    }
    loramodule[6]=0;
    
    if(strcmp(loramodule,"RN2903")==0){
      lora_value=0;
      return lora_value;
    }
    else if(strcmp(loramodule,"RN2483")==0){
      lora_value=1;
      return lora_value;
    }
    else{
      fprintf(stderr,"wrong lora module\n");
      close(fd);
      return lora_value;
    }
}

int rfdlora_module_ver(int fd){
    unsigned char buf[8192];
    char loramodule[7];
    for(int i=0;i<7;i++){
      loramodule[i]='\0';
    }
    //unsigned clr[3]={21,13,10};
    char reset[] = "sys get ver\r\n";
    write_all(fd, reset, strlen(reset)); // asks for lora radio version
    sleep(1); // give the radio the chance to respond
    ssize_t count = read_nonblock(fd,buf,8192);  // read reply module version firmware ...
    int lora_value=3;

    if (count<7) {
      // Failed to get enough data back from radio to have the model number
      return -1;
    }
    
    //get the lora module identifier for debug purpose
    for(int i=0;i<6;i++){ 
      loramodule[i]=buf[i];
    }
    loramodule[6]=0;
    
    //update the loramodule to know which module we are communicating with then return it
    if(strcmp(loramodule,"RN2903")==0){ 
      lora_value=0;
      return lora_value;
    }
    else if(strcmp(loramodule,"RN2483")==0){
      lora_value=1;
      return lora_value;
    }
    else{
      fprintf(stderr,"wrong lora module\n");
      close(fd);
      return lora_value;
    }
}

int rfdlora_module_firmware(int fd, char * firmware)
{
  unsigned char buf[8192];
  for(int i=0;i<=6;i++) firmware[i]=0;
  
  char reset[] = "sys get ver\r\n";
  write_all(fd, reset, strlen(reset)); // ask Lora radio for module and version
  sleep(1); // give the radio the chance to respond
  ssize_t count = read_nonblock(fd,buf,8192);  // read lora reply
  
  if (count<13) return -1;
  
  for(int i=0;i<5;i++){ //parse lora response to get only firmware version
    firmware[i]=buf[i+7];
  }
  firmware[6]=0;

  return 0;
}

int rfdlora_module_snr(int fd, char * snr){
    unsigned char buf[8192];
    for(int i=0;i<4;i++){
      snr[i]='\0';
    }
    char Ssnr[] = "radio get snr\r\n";
    write_all(fd, Ssnr, strlen(Ssnr)); // ask Lora radio the snr of last received packet
    sleep(1); // give the radio the chance to respond
    ssize_t count = read_nonblock(fd,buf,8192);  // read reply : SNR

    if (count<(7+3)) return -1;
    
    for(int i=0;i<3;i++){
      snr[i]=buf[i+7];
    }
    snr[4]=0;

    return 0;
}

int rfdlora_initialise(int fd, int lora_module)
{
  // XXX -- The radio setup should be done specifically for each region,
  // and the region code read from the serial eeprom, and pick the correct module
  // for the correct region + frequency selection.

  // XXX - Select correct module first.
  
  int count;
  unsigned char buf[8192];
  //char *setup_string[7];
  // Tell LoRa radio we want to know when various events occur.
  char *setup_string[7]={
      "radio set mod lora\r\n", // set radio in LoRa mode
      "radio set freq 868100000\r\n", // set frequency according to the module used
      "radio set sf sf12\r\n", // set the spread factor of the radio
      "radio set bw 125\r\n", // set the bandwidth of the radio, in KHz
      "radio set cr 4/5\r\n", // set the error correction rate of the radio
      "radio set prlen 8\r\n", // set the pre-amble length of the radio in Symbols/sec
      "radio set pwr 14\r\n", // set the transmit power of the radio
    };
  switch(lora_module){
  case 0 : //RN2903
    setup_string[1]="radio set freq 923300000\r\n"; // Register for phone messages
  break;
  case 1 : //RN2483
    setup_string[1]="radio set freq 433100000\r\n"; // Register for phone messages
  break;
  default :
    printf("wrong lora module given\n");
    
    return(-1);
  break;
  }
  int i;
  for(i=0;i<7;i++) {
    write(fd,setup_string[i],strlen(setup_string[i]));
    usleep(200000);
    count = read_nonblock(fd,buf,8192);  // read reply
    dump_bytes(stderr,setup_string[i],buf,count);
  }    
  
  return 0;
}
/*
int rfdlora_serviceloop(int fd)
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
	write(fd,"\r\n",2);
	
	snprintf(cmd,1024,"AXLINK%s\r\n",hf_stations[next_station].name);
	write(fd,cmd,strlen(cmd));
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
      write(fd,"AILTBL\r\n",8);
      last_link_probe_time=time(0);
    }
    break;
  case HF_CONNECTING:
    break;
  case HF_ALELINK:
    // Probe periodically with AILTBL to get link table, because the modem doesn't
    // preemptively tell us when we lose a link
    if (time(0)!=last_link_probe_time)  {
      write(fd,"AILTBL\r\n",8);
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

int lora_process_line(char *l)
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

int rfdlora_receive_bytes(unsigned char *bytes,int count)
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

int hfbarrett_send_packet(int fd,unsigned char *out, int len)
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
    count = read_nonblock(fd,buffer,8192);
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
      
      write_all(fd,message,strlen(message));

      // Any ALE send will take at least a second, so we can safely wait that long
      sleep(1);

      // Check that it gets accepted for TX. If we see EV04, then something is still
      // being sent, and we have to wait and try again.
      count = read_nonblock(fd,buffer,8192);
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
