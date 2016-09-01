/*
  Manage the connected HF radio according to the supplied configuration.

  Configuration files look like:

10% calling duty cycle 
station "101" 5 minutes every 2 hours
station "102" 5 minutes every 2 hours
station "103" 5 minutes every 2 hours

  The calling duty cycle is calculated on an hourly basis, and counts only connected
  time. Call connections will be limited to 20% of the time, so that there is ample
  opportunity for a station to listen for incoming connections.

  A 100% duty cycle will mean that this radio will never be able to receive calls,
  so a 50% duty cycle (or better 1/n) duty cycle is probably more appropriate.

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

// Barrett turnaround is longer, because it must include the TX time for the last
// sent fragment.
#define HF_BARRETT_TURNAROUND_TIME (20+random()%10)
#define HF_CODAN_TURNAROUND_TIME (10+random()%10)

#define HF_DISCONNECTED 1
#define HF_CALLREQUESTED 2
#define HF_CONNECTING 3
#define HF_ALELINK 4
#define HF_DISCONNECTING 5
#define HF_ALESENDING 6
#define HF_COMMANDISSUED 0x100

int hf_state=HF_DISCONNECTED;
int hf_link_partner=-1;

time_t hf_next_call_time=0;

time_t last_link_probe_time=0;

extern unsigned char my_sid[32];
extern char *my_sid_hex;
extern char *servald_server;
extern char *credential;
extern char *prefix;

time_t hf_next_packet_time=0;
time_t last_outbound_call=0;

int hf_callout_duty_cycle=0;
int hf_callout_interval=5; // minutes

struct hf_station {
  char *name;
  int link_time_target; // minutes
  int line_time_interval; // hours

  // Next target link time
  // (calculated using a pro-rata extension of line_time_interval based on the
  // duration of the last link).
  time_t next_link_time;

  // Time for next hangup, based on aiming for a call to have a maximum duration of
  // linke_time_target.
  time_t hangup_time;

  // How many successive failures in connecting to this station
  // (used to condition the selection of which station to talk to.  Basically if we
  // keep failing to connect, then we will be more likely to try other stations first)
  int consecutive_connection_failures;
};

#define MAX_HF_STATIONS 1024
struct hf_station hf_stations[MAX_HF_STATIONS];
int hf_station_count=0;

int has_hf_plan=0;

char barrett_link_partner_string[1024]="";

time_t last_ready_report_time=0;

int hf_radio_ready()
{
  if (time(0)>=hf_next_packet_time) {
    if (time(0)!=last_ready_report_time) {
      char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
      if (timestr[0]) timestr[strlen(timestr)-1]=0;
      if (hf_state==HF_ALELINK)
	fprintf(stderr,"  [%s] HF Radio cleared to transmit.\n",
		timestr);
    }
    last_ready_report_time=time(0);
    return 1;
  } else {
    if (time(0)!=last_ready_report_time) {
      char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
      if (timestr[0]) timestr[strlen(timestr)-1]=0;
      fprintf(stderr,"  [%s] Wait %ld more seconds to allow other side to send.\n",
	      timestr,hf_next_packet_time-time(0));
    }
    last_ready_report_time=time(0);
    return 0;
  }
}

int hf_next_station_to_call()
{
  int i;
  for(i=0;i<hf_station_count;i++) {
    if (time(0)>hf_stations[i].next_link_time) return i;
  }
  return random()%hf_station_count;
}


int hf_read_configuration(char *filename)
{  
  FILE *f=fopen(filename,"r");
  if (!f) {
    fprintf(stderr,"Could not read HF radio configuration from '%s'\n",filename);
    perror("fopen");
    exit(-1);
  }

  char line[1024];
  int offset;
  char station_name[1024];
  int minutes,hours,seconds;

  line[0]=0; fgets(line,1024,f);
  while(line[0]) {
    if ((line[0]=='#')||(line[0]<' ')) {
      // ignore blank lines and # comments
    } else if (sscanf(line,"wait %d seconds%n",&seconds,&offset)==1) {
      // Wait this long before making first call
      last_outbound_call=time(0)+seconds;
      hf_next_packet_time=time(0)+seconds;
    } else if (sscanf(line,"%d%% duty cycle%n",&hf_callout_duty_cycle,&offset)==1) {
      if (hf_callout_duty_cycle<0||hf_callout_duty_cycle>100) {
	fprintf(stderr,"Invalid call out duty cycle: Must be between 0%% and 100%%\n");
	fprintf(stderr,"  Offending line: %s\n",line);
	exit(-1);
      }
    } else if (sscanf(line,"call every %d minutes%n",&hf_callout_interval,&offset)==1) {
      if (hf_callout_interval<0||hf_callout_interval>10000) {
	fprintf(stderr,"Invalid call out interval: Must be between 0 and 10000 minutes\n");
	fprintf(stderr,"  Offending line: %s\n",line);
	exit(-1);
      }
    } else if (sscanf(line,"station \"%[^\"]\" %d minutes every %d hours",
		      station_name,&minutes,&hours)==3) {
      fprintf(stderr,"Registering station '%s' (%d minutes every %d hours)\n",
	      station_name,minutes,hours);
      if (hf_station_count<MAX_HF_STATIONS) {
	bzero(&hf_stations[hf_station_count],sizeof(struct hf_station));
	hf_stations[hf_station_count].name=strdup(station_name);
	hf_stations[hf_station_count].link_time_target=minutes;
	hf_stations[hf_station_count].line_time_interval=hours;
	hf_station_count++;
      } else {
	fprintf(stderr,"Too many HF stations. Reduce list or increase MAX_HF_STATIONS.\n");
	exit(-1);
      }
    } else {
      fprintf(stderr,"Unknown directive in HF radio plan file.\n");
      fprintf(stderr,"  Offending line: %s\n",line);
      exit(-1);	
    }
    line[0]=0; fgets(line,1024,f);
  }
  fclose(f);

  has_hf_plan=1;
  fprintf(stderr,"Configured %d stations.\n",hf_station_count);
  
  return 0;
}

int hf_serviceloop(int serialfd)
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
	if (radio_get_type()==RADIO_CODAN_HF) {
	  snprintf(cmd,1024,"alecall %s \"!SERVAL,1,0,%s\"\r\n",
		   hf_stations[next_station].name,
		   radio_get_type()==RADIO_CODAN_HF?"CODAN":"BARRETT");
	  write(serialfd,cmd,strlen(cmd));
	  hf_link_partner=next_station;
	  hf_state = HF_CALLREQUESTED|HF_COMMANDISSUED;
	} else {
	  // Ensure we have a clear line for new command (we were getting some
	  // errors here intermittantly).
	  write(serialfd,"\r\n",2);
	  
	  snprintf(cmd,1024,"AXLINK%s\r\n",hf_stations[next_station].name);
	  write(serialfd,cmd,strlen(cmd));
	  hf_state = HF_CALLREQUESTED;
	}
	fprintf(stderr,"HF: Attempting to call station #%d '%s'\n",
		next_station,hf_stations[next_station].name);
      }
    }
    break;
  case HF_CALLREQUESTED:
    if (radio_get_type()==RADIO_BARRETT_HF) {
      // Probe periodically with AILTBL to get link table, because the modem doesn't
      // preemptively tell us when we get a link established
      if (time(0)!=last_link_probe_time)  {
	write(serialfd,"AILTBL\r\n",8);
	last_link_probe_time=time(0);
      }
    }
    break;
  case HF_CONNECTING:
    break;
  case HF_ALELINK:
    if (radio_get_type()==RADIO_BARRETT_HF) {
      // Probe periodically with AILTBL to get link table, because the modem doesn't
      // preemptively tell us when we lose a link
      if (time(0)!=last_link_probe_time)  {
	write(serialfd,"AILTBL\r\n",8);
	last_link_probe_time=time(0);
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

int nybltohexchar(int v)
{
  if (v<10) return '0'+v;
  return 'A'+v-10;
}

int ishex(int c)
{
  if (c>='0'&&c<='9') return 1;
  if (c>='A'&&c<='F') return 1;
  if (c>='a'&&c<='f') return 1;
  return 0;
}

int chartohexnybl(int c)
{
  if (c>='0'&&c<='9') return c-'0';
  if (c>='A'&&c<='F') return c-'A'+10;
  if (c>='a'&&c<='f') return c-'a'+10;
  return 0;
}


int hex_encode(unsigned char *in, char *out, int in_len, int radio_type)
{
  int out_ofs=0;
  int i;
  for(i=0;i<in_len;i++) {
    out[out_ofs++]=nybltohexchar(in[i]>>4);
    out[out_ofs++]=nybltohexchar(in[i]&0xf);
  }
  out[out_ofs]=0;
  return out_ofs;
}

int hex_decode(char *in, unsigned char *out, int out_len,int radio_type)
{
  int i;
  int out_count=0;

  for(i=0;i<strlen(in);i+=2) {
    int v=hextochar(in[i+0])<<4;
    v|=hextochar(in[i+1]);
    out[out_count++]=v;
  }
  out[out_count]=0;
  return out_count;
}

int ascii64_encode(unsigned char *in, char *out, int in_len, int radio_type)
{
  // ASCII-64 is use by HF ALE radio links. It is just ASCII codes 0x20 - 0x5f
  // On Barrett, nothing is escaped.
  // On Codan, spaces must be escaped

  int out_ofs=0;
  int i,j;
  for(i=0;i<in_len;i+=3) {
    // Encode 3 bytes using 4
    unsigned char ob[4];
    ob[0]=0x20+(in[i+0]&0x3f);
    ob[1]=0x20+((in[i+0]&0xc0)>>6)+((in[i+1]&0x0f)<<2);
    ob[2]=0x20+((in[i+1]&0xf0)>>4)+((in[i+2]&0x03)<<4);
    ob[3]=0x20+((in[i+2]&0xfc)>>2);

    for(j=0;j<4;j++) {
      if ((ob[j]==' ')&&(radio_type==RADIO_CODAN_HF)) {
	out[out_ofs++]='\\';
      }
      out[out_ofs++]=ob[j];
    }
  }
  out[out_ofs]=0;
  return 0;
}

int ascii64_decode(char *in, unsigned char *out, int out_len,int radio_type)
{
  int i;
  int out_ofs=0;
  for(i=0;in[i];i+=4) {
    unsigned char ob[3];
    ob[0]=(in[i+0]-0x20)&0x3f;
    ob[0]|=(((in[i+1]-0x20)&0x03)<<6);
    ob[1]=(((in[i+1]-0x20)&0x3c)>>2);
    ob[1]|=(((in[i+2]-0x20)&0x0f)<<4);
    ob[2]=(((in[i+2]-0x20)&0x30)>>4);
    ob[2]|=(((in[i+3]-0x20)&0x3f)<<2);
    out_ofs+=3;
  }

  return out_ofs;
}

int pieces_seen[6]={0,0,0,0,0,0};
unsigned char accummulated_packet[256];

char *radio_type_name(int radio_type)
{
  switch (radio_type) {
  case RADIO_CODAN_HF: return "Codan HF";
  case RADIO_BARRETT_HF: return "Barrett HF";
  default: return "Unknown";
  }
}

int hf_process_fragment(char *fragment)
{
  int peer_radio=-1;
  int sequence=-1;
  if ((fragment[0]>='0')&&(fragment[0]<='7')) {
    peer_radio=RADIO_CODAN_HF;
    sequence=fragment[0]-'0';
  }
  if ((fragment[0]>='A')&&(fragment[0]<='H')) {
    peer_radio=RADIO_BARRETT_HF;
    sequence=fragment[0]-'A';
  }
  int piece_number=(fragment[1]-'0');
  int pieces=(fragment[2]-'0');

  fprintf(stderr,"Checking if message is a fragment (piece %d/%d, peer=%d).\n",
	  piece_number,pieces,peer_radio);
  if (peer_radio<0) return -1;
  if (pieces<1||pieces>6) return -1;
  if (piece_number<0||piece_number>5) return -1;
  fprintf(stderr,"Received piece %d/%d of packet sequence #%d from a %s radio.\n",
	  piece_number+1,pieces,sequence,radio_type_name(peer_radio));

  int packet_offset=piece_number*43;
  int i;
  for(i=3;i<strlen(fragment);i+=2) {
    if (ishex(fragment[i+0])&&ishex(fragment[i+1])) {
      int v=(chartohexnybl(fragment[i+0])<<4)+chartohexnybl(fragment[i+1]);
      accummulated_packet[packet_offset++]=v;
    }
  }
  if (piece_number==(pieces-1)) {
    // We have a terminal piece: so assume we have the whole packet.
    // (the FEC will reject it if it is incorrectly assembled).
    fprintf(stderr,"Passing reassembled packet of %d bytes up for processing.\n",
	    packet_offset);
    saw_packet(accummulated_packet,packet_offset,
	       my_sid_hex,prefix,servald_server,credential);

    // Now it is our turn to send
    hf_radio_send_now();
  } else
    // Not end of packet, wait 8+1d8 seconds before we try transmitting.
    hf_radio_pause_for_turnaround();
  
  return 0;
}

int ale_inprogress=0;

int hf_codan_process_line(char *l)
{
  int channel,caller,callee,day,month,hour,minute;
  
  //  fprintf(stderr,"Codan radio (state 0x%04x) says: %s\n",hf_state,l);
  if (hf_state&HF_COMMANDISSUED) {
    // Ignore echoed commands, and wait for ">" prompt
    if (l[0]=='>') hf_state&=~HF_COMMANDISSUED;
    else if (!strcmp(l,"CALL STARTED")) {
      hf_state=HF_COMMANDISSUED|HF_CONNECTING;
    }
  }

  char fragment[8192];
  
  if (!strcmp(l,"AMD CALL STARTED")) ale_inprogress=1;
  else if (!strcmp(l,"CALL DETECTED")) {
    // Incoming ALE message -- so don't try sending anything for a little while
    hf_radio_pause_for_turnaround();
  } else if (!strcmp(l,"AMD CALL FINISHED")) ale_inprogress=0;
  else if (sscanf(l,"AMD-CALL: %d, %d, %d, %d/%d %d:%d, \"%[^\"]\"",
		  &channel,&caller,&callee,&day,&month,&hour,&minute,fragment)==8) {
    // Saw a fragment
    hf_process_fragment(fragment);
    // We must also by definition be connected
    hf_state=HF_ALELINK;
  } else if (sscanf(l,"ALE-LINK: %d, %d, %d, %d/%d %d:%d",
	     &channel,&caller,&callee,&day,&month,&hour,&minute)==7) {
    if (hf_link_partner>=-1)
      hf_stations[hf_link_partner].consecutive_connection_failures=0;
    ale_inprogress=0;
    if ((hf_state&0xff)!=HF_CONNECTING) {
      // We have a link, but without us asking for it.
      // So allow 10 seconds before trying to TX, else we start TXing immediately.
      hf_radio_pause_for_turnaround();
    } else hf_radio_send_now();

    fprintf(stderr,"ALE Link from %d -> %d on channel %d, I will send a packet in %ld seconds\n",
	    caller,callee,channel,
    	    hf_next_packet_time-time(0));

    hf_state=HF_ALELINK;    
  } else if ((!strcmp(l,"ALE-LINK: FAILED"))||(!strcmp(l,"LINK: CLOSED"))) {
    if (hf_state==HF_ALELINK) {
      // disconnected
    }
    if ((!strcmp(l,"ALE-LINK: FAILED"))||(hf_state!=HF_CONNECTING)) {
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
      hf_state=HF_DISCONNECTED|HF_COMMANDISSUED;
    }
  }
  
  return 0;
}

int hf_barrett_process_line(char *l)
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
    } else hf_radio_send_now();

    
    fprintf(stderr,"ALE Link established with %s (station #%d), I will send a packet in %ld seconds\n",
	    barrett_link_partner_string,hf_link_partner,
	    hf_next_packet_time-time(0));
    
    hf_state=HF_ALELINK;
  }

  return 0;
}


char hf_response_line[1024];
int hf_rl_len=0;

int hf_codan_receive_bytes(unsigned char *bytes,int count)
{ 
  int i;
  for(i=0;i<count;i++) {
    if (bytes[i]==13||bytes[i]==10) {
      hf_response_line[hf_rl_len]=0;
      if (hf_rl_len) hf_codan_process_line(hf_response_line);
      hf_rl_len=0;
    } else {
      if (hf_rl_len<1024) hf_response_line[hf_rl_len++]=bytes[i];
    }
  }
  
  return 0;
}

int hf_barrett_receive_bytes(unsigned char *bytes,int count)
{
  int i;
  for(i=0;i<count;i++) {
    if (bytes[i]==13||bytes[i]==10) {
      hf_response_line[hf_rl_len]=0;
      if (hf_rl_len) hf_barrett_process_line(hf_response_line);
      hf_rl_len=0;
    } else {
      if (hf_rl_len<1024) hf_response_line[hf_rl_len++]=bytes[i];
    }
  }
  return 0;
}

int message_sequence_number=0;

int radio_send_message_codanhf(int serialfd,unsigned char *out, int len)
{
  // We can send upto 90 ALE encoded bytes.  ALE bytes are 6-bit, so we can send
  // 22 groups of 3 bytes = 66 bytes raw and 88 encoded bytes.  We can use the first
  // two bytes for fragmentation, since we would still like to support 256-byte
  // messages.  This means we need upto 4 pieces for each message.
  char message[8192];
  char fragment[8192];

  int i;
  time_t absolute_timeout=time(0)+90;

  if (hf_state!=HF_ALELINK) {
    fprintf(stderr,"Not sending packet, because we don't think we are in an ALE link.\n");
    return -1;
  }
  if (ale_inprogress) {
    fprintf(stderr,"Not sending packet, because we think an ALE transaction is already occurring.\n");
    return -1;
  }

  // How many pieces to send (1-6)
  // This means we have 36 possible fragment indications, if we wish to imply the
  // number of fragments in the fragment counter.
  int pieces=len/43; if (len%43) pieces++;
  
  fprintf(stderr,"Sending message of %d bytes via Codan HF\n",len);
  for(i=0;i<len;i+=43) {
    // Indicate radio type in fragment header
    fragment[0]=0x30+(message_sequence_number&0x07);
    fragment[1]=0x30+(i/43);
    fragment[2]=0x30+pieces;
    int frag_len=43; if (len-i<43) frag_len=len-i;
    hex_encode(&out[i],&fragment[3],frag_len,radio_get_type());
    
    snprintf(message,8192,"amd %s\r\n",fragment);
    write_all(serialfd,message,strlen(message));

    int not_ready=1;
    while (not_ready) {
      if (time(0)>absolute_timeout) {
	fprintf(stderr,"Failed to send packet in reasonable amount of time. Aborting.\n");
	message_sequence_number++;
	return -1;
      }

      usleep(100000);

      unsigned char buffer[8192];
      int count = read_nonblock(serialfd,buffer,8192);
      // if (count) dump_bytes("postsend",buffer,count);
      if (count) hf_codan_receive_bytes(buffer,count);
      if (strstr((const char *)buffer,"AMD CALL FINISHED")) {
	not_ready=0;
	char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
	if (timestr[0]) timestr[strlen(timestr)-1]=0;
	fprintf(stderr,"  [%s] Sent %s",timestr,message);

      } else not_ready=1;
      if (strstr((const char *)buffer,"ERROR")) {
	// Something went wrong
	fprintf(stderr,"Error sending packet: Aborted.\n");
	message_sequence_number++;
	return -1;
      }
      
    }    
  }
  
  hf_radio_pause_for_turnaround();
  message_sequence_number++;
  char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
  if (timestr[0]) timestr[strlen(timestr)-1]=0;
  fprintf(stderr,"  [%s] Finished sending packet, next in %ld seconds.\n",
	  timestr,hf_next_packet_time-time(0));
  
  return 0;
}

int radio_send_message_barretthf(int serialfd,unsigned char *out, int len)
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
    fragment[0]=0x41+(message_sequence_number&0x07);
    fragment[1]=0x30+(i/43);
    fragment[2]=0x30+pieces;
    int frag_len=43; if (len-i<43) frag_len=len-i;
    hex_encode(&out[i],&fragment[3],frag_len,radio_get_type());

    unsigned char buffer[8192];
    int count;

    usleep(100000);
    count = read_nonblock(serialfd,buffer,8192);
    if (count) dump_bytes("presend",buffer,count);
    if (count) hf_barrett_receive_bytes(buffer,count);
    
    snprintf(message,8192,"AXNMSG%s%02d%s\r\n",
	     barrett_link_partner_string,
	     (int)strlen(fragment),fragment);

    int not_accepted=1;
    while (not_accepted) {
      if (time(0)>absolute_timeout) {
	fprintf(stderr,"Failed to send packet in reasonable amount of time. Aborting.\n");
	message_sequence_number++;
	return -1;
      }
      
      write_all(serialfd,message,strlen(message));

      // Any ALE send will take at least a second, so we can safely wait that long
      sleep(1);

      // Check that it gets accepted for TX. If we see EV04, then something is still
      // being sent, and we have to wait and try again.
      count = read_nonblock(serialfd,buffer,8192);
      // if (count) dump_bytes("postsend",buffer,count);
      if (count) hf_barrett_receive_bytes(buffer,count);
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
  message_sequence_number++;
  char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
  if (timestr[0]) timestr[strlen(timestr)-1]=0;
  fprintf(stderr,"  [%s] Finished sending packet, next in %ld seconds.\n",
	  timestr,hf_next_packet_time-time(0));
  
  return 0;
}

int hf_radio_send_now()
{
  hf_next_packet_time=0;
  char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
  if (timestr[0]) timestr[strlen(timestr)-1]=0;
  fprintf(stderr,"  [%s] It is our turn to send.\n",timestr);
  return 0;

}

int hf_radio_pause_for_turnaround()
{
  switch(radio_get_type()) {
  case RADIO_BARRETT_HF:
    hf_next_packet_time=time(0)+HF_BARRETT_TURNAROUND_TIME;
    break;
  case RADIO_CODAN_HF:
    hf_next_packet_time=time(0)+HF_CODAN_TURNAROUND_TIME;
    break;
  default:
    fprintf(stderr,"Unknown radio type 0x%04x\n",radio_get_type());
    break;
  }
  char timestr[100]; time_t now=time(0); ctime_r(&now,timestr);
  if (timestr[0]) timestr[strlen(timestr)-1]=0;
  fprintf(stderr,"  [%s] Delaying %ld seconds to allow other side to send.\n",
	  timestr,hf_next_packet_time-time(0));
  return 0;
}
