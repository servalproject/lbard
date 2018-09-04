/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: ALORA,"rfdlora","RFD Tri-Band LoRa module",rfdlora_radio_detect,rfdlora_serviceloop,rfdlora_receive_bytes,rfdlora_send_packet,rfdlora_check_if_ready,20
*/
#define _POSIX_SOURCE
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
#include <sys/stat.h>

#include "sync.h"
#include "lbard.h"
#include "hf.h"
#include "radios.h"

int rfdlora_initialise(int fd, int lora_module); 
int rfdlora_switch_module(int fd);
int rfdlora_module_reset(int fd);
int rfdlora_break(int fd);
int rfdlora_module_ver(int fd);
int rfdlora_module_firmware(int fd, char* firmware);
int rfdlora_module_snr(int fd, char * snr);
void rfdlora_send_packets(int fd);
void rfdlora_receive_packets(int fd);
int same_file(int fd1, int fd2);
// Import serial_port string from main.c
extern char *serial_port;
extern int serialfd;
int rfdlora_radio_detect(int fd)
{
  if (fd==-1){
    fprintf(stderr,"No serial port, so no radio\n");
    return 0;
  }
  else{ 
    // set the serial port speed to 57600, required for lora radios
    serial_setup_port_with_speed(fd,57600);
    // call a function to get the lora radio out of non responsive state
    if(rfdlora_break(fd)==0){
      fprintf(stderr,"non responsive state break failed\n");
      return 0;
    }
    // reset the lora radio we are communicating with and retrieve module identifier (RN2903 or RN4843)
    int lora_value = rfdlora_module_reset(fd); 
    // manage the wrong value return case
    if (lora_value==-1){
      fprintf(stderr,"reset failed\n");
      return 0;
    }else{
      //set lora radio parameters according to region and module given
      if(rfdlora_initialise(fd,lora_value)==-1){ 
        fprintf(stderr,"init failed\n");
        return 0;
      }
      else{
        fprintf(stderr,"First module initialized\n");
        fprintf(stderr,"Switching to other module\n");
        // switch to the other lora radio module 
        rfdlora_switch_module(fd); 
        // reset the lora radio we are communicating with and retrieve module identifier (RN2903 or RN4843)
        lora_value = rfdlora_module_reset(fd);
        // manage the wrong value return case
        if (lora_value==-1){
          fprintf(stderr,"reset failed\n");
          return 0;
        }else{
          //set lora radio parameters according to region and module given
          if(rfdlora_initialise(fd,lora_value)==-1){ 
            fprintf(stderr,"init failed\n");
            return 0;
          }
          else{
            fprintf(stderr,"Second module initialized\n");
          }
          //get lora module name (RN2903 or RN4843)
          int version = rfdlora_module_ver(fd); 
          char firmware[1024] = {0};
          //get lora module firmware version
          rfdlora_module_firmware(fd, firmware); 
          fprintf(stderr,"module version : %d  -- 0 = RN2903 and 1 = RN2483\n",version);
          fprintf(stderr,"module firmware : %s\n", firmware);
          radio_set_type(RADIOTYPE_ALORA);
          return 1;
        }
      }
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

int rfdlora_switch_module(int fd){
  char pin[] = "sys get pindig GPIO13\r\n";
  unsigned char buf[8192];
  // get value of GPIO pin 13 = used to switch the radio we are communicating with
  write_all(fd, pin, strlen(pin)); 
  usleep(200000);
  int count=read_nonblock(fd,buf,8192);  // read and ignore any stuff
  dump_bytes(stderr,"bytes following CRLF",buf,count);
  // -48 is for transforming the char in int : in the ASCII table "0" = 48 so 48-48 = 0 and "1" = 49 so 49-48 =1.
  int GPIO13 = buf[0] - 48; 
  fprintf(stderr,"GPIO 13 value obtained : %d. Now reverting it.\n", GPIO13);
  if (GPIO13==0){
    char switchm[] = "sys set pindig GPIO13 1\r\n";
    write_all(fd, switchm, strlen(switchm)); // change value of GPIO pin 13 = switch the radio we are communicating with
    //fprintf(stderr,"\n\n-----------------------------------------------------------------------\n\n |%s| \n\n-----------------------------------------------------------------------\n\n",buf);
    return 0;
  }else if (GPIO13==1){
    char switchm[] = "sys set pindig GPIO13 0\r\n";
    write_all(fd, switchm, strlen(switchm)); // ask Lora radio for module and version strlen(init)
    //fprintf(stderr,"\n\n-----------------------------------------------------------------------\n\n |%s| \n\n-----------------------------------------------------------------------\n\n",buf);
    return 0;
  }else{
    fprintf(stderr,"Wrong GPIO13 value");
    return -1;
  }
  //return 0;
}

int rfdlora_break(int fd){
  // Wait for all data transmission to the terminal to finish 
  // and then transmit a break condition to the terminal.     
    unsigned char buf[8192];
    char reset[] = "U\r\n"; 
    int timer = 0;
    // wait for the radio to be available for communication
    if (tcdrain(fd) != 0) {
      perror("tcdrain error");
      return(0);
    }
    else {
      // send a break signal to the module
      if (tcsendbreak(fd, timer) != 0){
        fprintf(stderr,"break : tcsendbreak() error, %d\n", errno);
        return 0;
      }
      else{
        // send 0x55 in order to get it out from non responsive state
        write_all(fd, reset, strlen(reset)); 
        usleep(2000000);
        int count=read_nonblock(fd,buf,8192);  // read and ignore any stuff
        dump_bytes(stderr,"bytes following break and 0x55",buf,count);
        return 1;
      }
    }
  return 0;
}
int rfdlora_module_reset(int fd){
    fprintf(stderr,"Entered rfdlora_module_reset()\n");
    unsigned char buf[8192];
    char loramodule[7];
    for(int i=0;i<7;i++){
      loramodule[i]='\0';
    }
    char reset[] = "sys reset\r\n";
    fprintf(stderr,"Writing CRLF to modem.\n");
    // Clear any partial command
    write_all(fd,"\r\n",2); 
    usleep(2000000);
    int count=read_nonblock(fd,buf,8192);  // read and ignore any stuff
    dump_bytes(stderr,"bytes following CRLF",buf,count);
    fprintf(stderr,"Writing reset command to modem.\n");
    write_all(fd, reset, strlen(reset)); // reset Lora radio 
    int lora_value=3;

    //not working with smaller value of usleep here, the module did not have the time to reset and therefore didn't send any response in time
    usleep(8000000); 
    count=read_nonblock(fd,buf,8192);
    dump_bytes(stderr,"bytes following reset",buf,count);

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
      fprintf(stderr,"wrong lora module : %s\n", loramodule);
      return -1;
    }
}

int rfdlora_module_ver(int fd){
    unsigned char buf[8192];
    char loramodule[7];
    for(int i=0;i<7;i++){
      loramodule[i]='\0';
    }
    char reset[] = "sys get ver\r\n";
    // ask for lora module version
    write_all(fd, reset, strlen(reset)); 
    // give the radio the chance to respond
    sleep(1); 
    // read reply module version firmware ...
    ssize_t count = read_nonblock(fd,buf,8192); 
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
      fprintf(stderr,"rfdlora_module_ver : wrong lora module\n");
      close(fd);
      return lora_value;
    }
}

int rfdlora_module_firmware(int fd, char * firmware)
{
  unsigned char buf[8192];
  for(int i=0;i<=6;i++) firmware[i]=0;
  
  char reset[] = "sys get ver\r\n";
  // ask Lora radio for module and version
  write_all(fd, reset, strlen(reset)); 
  // give the radio the chance to respond
  sleep(1);
  // read lora reply
  ssize_t count = read_nonblock(fd,buf,8192);  
  
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
  // Tell LoRa radio we want to know when various events occur.
  char *setup_string[7]={
      "radio set mod lora\r\n", // set radio in LoRa mode
      "radio set freq xxxx00000\r\n", // set frequency according to the module used
      "radio set sf sf12\r\n", // set the spread factor of the radio
      "radio set bw 125\r\n", // set the bandwidth of the radio, in KHz
      "radio set cr 4/5\r\n", // set the error correction rate of the radio
      "radio set prlen 8\r\n", // set the pre-amble length of the radio in Symbols/sec
      "radio set pwr 14\r\n", // set the transmit power of the radio
    };
  switch(lora_module){
  case 0 : //RN2903
    setup_string[1]="radio set freq 923300000\r\n"; // frequency chosen for RN2903 according to region
  break;
  case 1 : //RN2483
    setup_string[1]="radio set freq 433100000\r\n"; // frequency chosen for RN2483 according to region
  break;
  default :
    fprintf(stderr,"rfdlora_initialise : wrong lora module given : %d\n", lora_module);
    
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

// used to compare if 2 files descriptor are pointing on the same file
/*int same_file(int fd1, int fd2) {
    struct stat stat1, stat2;
    if(fstat(fd1, &stat1) < 0) return -1;
    if(fstat(fd2, &stat2) < 0) return -1;
    return (stat1.st_dev == stat2.st_dev) && (stat1.st_ino == stat2.st_ino);
}*/