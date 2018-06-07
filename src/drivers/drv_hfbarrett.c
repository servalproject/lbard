
/*
The following specially formatted comments tell the LBARD build environment about this radio.
See radio_type for the meaning of each field.
See radios.h target in Makefile to see how this comment is used to register support for the radio.

RADIO TYPE: HFBARRETT,"hfbarrett","Barrett HF with ALE",hfcodanbarrett_radio_detect,hfbarrett_serviceloop,hfbarrett_receive_bytes,hfbarrett_send_packet,hf_radio_check_if_ready,20

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

char barrett_link_partner_string[1024]="";
int previous_state=-1;
unsigned char pause_tx=0x013; //XOFF by default
time_t ALElink_establishment_time=25; //should get it from the alias?
int ale_link_just_established;

int hfbarrett_initialise(int serialfd)
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
  char *setup_string[8]
    ={
		"AIATBL\r\n", //Ask for all valid ale addresses
    "ARAMDM1\r\n", // Register for AMD messages
    "ARAMDP1\r\n", // Register for phone messages
    "ARCALL1\r\n", // Register for new calls
    "ARLINK1\r\n", // Hear about ALE link notifications
    "ARLTBL1\r\n", // Hear about ALE link table events
    "ARMESS1\r\n", // Hear about ALE event notifications
    "ARSTAT1\r\n", // Hear about ALE status change notifications
  };
  int i;
  for(i=0; i<8; i++) {
    write(serialfd,setup_string[i],strlen(setup_string[i]));
    usleep(200000);
 //   count = read_nonblock(serialfd,buf,8192);  // read reply
 //   dump_bytes(stderr,setup_string[i],buf,count);
  }    
  
  return 1;
}

int hfbarrett_serviceloop(int serialfd)
{
	char cmd[1024];
  
  switch(hf_state) {

  case HF_DISCONNECTED: //1
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
	
				snprintf(cmd,1024,"AXLINK%s%s\r\n", hf_stations[next_station].index, self_hf_station.index);
				//printf("sending '%s' to try to make ALE call.\n",cmd);
				write(serialfd,cmd,strlen(cmd));

				hf_state = HF_CALLREQUESTED;
						
				fprintf(stderr,"HF: Attempting to call station #%d '%s'\n",
			next_station,hf_stations[next_station].name);
      	hf_next_call_time=time(0)+ALElink_establishment_time;
				
				
			} 				
    }
		// Probe periodically with AILTBL to get link table, because the modem doesn't
    // preemptively tell us when we get a link established 
		else if (time(0)!=last_link_probe_time) { //once a second
      write(serialfd,"AILTBL\r\n",8);
      last_link_probe_time=time(0);
    }
    break;

  case HF_CALLREQUESTED: //2
		// Probe periodically with AILTBL to get link table, because the modem doesn't
    // preemptively tell us when we get a link established
    if (time(0)!=last_link_probe_time)  { //once a second
      write(serialfd,"AILTBL\r\n",8);
      last_link_probe_time=time(0);
    }
		if (time(0)>=hf_next_call_time){ //no reply from the called station
			hf_state = HF_DISCONNECTED;
			printf("Make the call disconnected because of no reply\n");
		}
    break;

  case HF_CONNECTING: //3
		
    break;

  case HF_ALELINK: //4
		// Probe periodically with AILTBL to get link table, because the modem doesn't
    // preemptively tell us when we lose a link
		if ((ale_link_just_established==1)){
			// Abort the link to ba able to send message
			// The link establishment using AXLINK is used to synchronise the 2 lbards
			// But this link establishment prevent the radio to properly send messages
			// So once the link is established using AXLINK, lbard ask it to terminate
			// Messages and call can still be sent and receive without this link establishment

	    printf("Abort the ALE link established with the AXLINK command to be able to send ALE messages\n");
      write_all(serialfd,"AXTLNK00\r\n",10); 
			sleep(10); //Sleep during the time the link terminates
			ale_link_just_established=0;
		}
    if (time(0)!=last_link_probe_time)  { //once a second
			last_link_probe_time=time(0);
    }
    break;

  case HF_DISCONNECTING: //5
		
    break;

	case HF_ALESENDING: //6

		break;

  default:
		
    break;
  }

	if (previous_state!=hf_state){
		fprintf(stderr,"\nBarrett radio changed to state 0x%04x\n",hf_state);
		previous_state=hf_state;
  }
  return 0;
}

int hfbarrett_process_line(char *l)
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
	if ((!strcmp(l,"EV08"))&&(hf_state==HF_ALELINK)) {
    // Unknown error but tests have shown it is because the radio is receiving or transmiting
    //printf("Saw EV08 response. Sleep for 4 seconds\n");
		//sleep(4);

	  // If we see this error during an ALE call, it means that the radio has got confused, and
    // will not send any more messages until we disconnect and reconnect the ALE link. So we
    // will immediately disconnect.
    hf_state=HF_ALELINKCONFUSED;
    return 0;
  }
  if ((!strcmp(l,"EV07"))&&(hf_state==HF_ALELINK)) {
    // If we see this error during an ALE call, it means that the radio has got confused, and
    // will not send any more messages until we disconnect and reconnect the ALE link. So we
    // will immediately disconnect.
    hf_state=HF_ALELINKCONFUSED;
    return 0;
  }

  char tmp[8192];

	if (sscanf(l, "AIATBL%s", tmp)==1){ 
		//AIATBL[II][T][LL][Alias]
		//where:
		//[II](00-99) Address index in ALE memory map
		//[T]Address type (1: self, 2: other) 
		//[LL](00-15) Address alias length
		//[Alias]Address alias (up to 15 basic 38 ASCII subset characters)

		hf_parse_linkcandidate(l);
		
		//display all the hf radios
		printf("The self hf Barrett radio is: \n%s, index=%s\n", self_hf_station.name, self_hf_station.index);		
		printf("The registered stations are:\n");		
		int i;		
		for (i=0; i<hf_station_count; i++){
			printf("%s, index=%s\n", hf_stations[i].name, hf_stations[i].index);
		}

		//Only with one radio registered
		if(hf_station_count==1){
			init_buffer(barrett_link_partner_string, 5);
			strcpy(barrett_link_partner_string, hf_stations[0].index);
			strcat(barrett_link_partner_string, self_hf_station.index);
			hf_link_partner=0;
			printf("link partner is: %s\n", barrett_link_partner_string);
		}
		else{
			printf("More than one radio registered on the radio, the code need to be adapted\n");
			exit(-1);
		}
	}

  if (sscanf(l,"AIAMDM%s",tmp)==1) {
    fprintf(stderr,"Barrett radio saw ALE AMD message '%s'\n",&tmp[6]);
    hf_process_fragment(&tmp[6]);
  }

	if ((sscanf(l, "AISTAT%s", tmp)==1) &&(hf_state==HF_CALLREQUESTED) ){
		if (tmp[1]!='1'){
			printf("The radio is not transmiting the call. Marking sent call disconnected");
			hf_state=HF_DISCONNECTED;
		}
	}
/*
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
      printf("Timed out trying to connect. Marking call disconnected.\n");
      hf_state=HF_DISCONNECTED;
  } */
	else if ((sscanf(l,"AILTBL%s",tmp)==1)&&(hf_state!=HF_ALELINK)) {
  /*  // Link established
    barrett_link_partner_string[0]=tmp[4];
    barrett_link_partner_string[1]=tmp[5];
    barrett_link_partner_string[2]=tmp[2];
    barrett_link_partner_string[3]=tmp[3];
    barrett_link_partner_string[4]=0;
	
    int i;
    hf_link_partner=-1;
    for(i=0;i<hf_station_count;i++){
			strcpy(tmp, hf_stations[i].index);
			strcat(tmp, self_hf_station.index);
      if (!strcmp(barrett_link_partner_string, tmp)){ 
				hf_link_partner=i;
				hf_stations[hf_link_partner].consecutive_connection_failures=0;
			  break; 
			}
		}*/

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
		ale_link_just_established=1;
  }

  return 0;
}

int hfbarrett_receive_bytes(unsigned char *bytes,int count)
{
  int i;
  for(i=0;i<count;i++) {
    if (bytes[i]==0x011){
			//printf("XON\n");
			pause_tx=0x011;
		}
		if (bytes[i]==0x013){
			//printf("XOFF\n");
			pause_tx=0x013;
		}
    if (bytes[i]==13||bytes[i]==10) { //end of command detected => if not null, line is processed by lbard
      hf_response_line[hf_rl_len]=0; //	after the command we out a '\0' to have a proper string
      if (hf_rl_len){ hfbarrett_process_line(hf_response_line);}
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
		printf("Nb of loops=%d\n", i);
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

    //if (count) dump_bytes(stderr,"presend",buffer,count);
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
			count = read_nonblock(serialfd,buffer,8192);
			if (count) hfbarrett_receive_bytes(buffer,count);
			if (pause_tx==0x011){
		    printf("Atempting to send one fragment: %s", message);

        if (hf_state==HF_ALELINKCONFUSED) {
          printf("Trying to unconfuse ALE link status\n");
	        write_all(serialfd,"AXTLNK00\r\n",10);
          hf_state=HF_DISCONNECTED;
          sleep(2);
        }

		    write_all(serialfd,message,strlen(message));

        count=-1;
        //for (int n=0;n<24&&count<1;n++) {
				for (int n=0;n<27;n++) {
				  // Any ALE send will take at least a second, so we can safely wait that long
				  sleep(1);
		
				  // Check that it gets accepted for TX. If we see EV04, then something is still
				  // being sent, and we have to wait and try again.
					init_buffer(buffer, 8192);
				  count = read_nonblock(serialfd,buffer,8192);
  if (count) dump_bytes(stderr,"during tx",buffer,count);
					if (count) hfbarrett_receive_bytes(buffer,count);
					if (strstr((const char *)buffer,"AISTAT20")&&(n<3)){
						printf("Radio turned into idle before sending message\n");
						break;
					}
					else if (strstr((const char *)buffer,"AISTAT20")){
						not_accepted=0;
						char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
						if (timestr[0]) timestr[strlen(timestr)-1]=0;
						fprintf(stderr,"  [%s] Sent %s",timestr,message);
						break;
					}        
				}
 sleep(10);

		/*    //if (count) dump_bytes(stderr,"postsend",buffer,count);
		    if (count) hfbarrett_receive_bytes(buffer,count);
		    if (strstr((const char *)buffer,"OK")
			&&(!strstr((const char *)buffer,"EV"))) {
		not_accepted=0;
		char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
		if (timestr[0]) timestr[strlen(timestr)-1]=0;
		fprintf(stderr,"  [%s] Sent %s",timestr,message);

		    } else {
					not_accepted=1;
					printf("Message not accepted by the radio\n");
				}  */    
    	} else{
				not_accepted=1;
				printf("\nThe radio is not ready to receive a command (XOFF)\n ");				
				sleep(1);				
			}
		}    
  }
  hf_radio_pause_for_turnaround();
  hf_message_sequence_number++;
  char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
  if (timestr[0]) timestr[strlen(timestr)-1]=0;
  fprintf(stderr,"  [%s] Finished sending packet, next in %ld seconds.\n",
	  timestr,hf_next_packet_time-time(0));
  
  return 0;
}

int init_buffer(unsigned char* buffer, int size){
	int i;
	for(i=0; i<size; i++){
		if (buffer[i]!=0)
			buffer[i]=0;
	}
	return 0; 
}
