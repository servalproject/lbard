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

#include "sync.h"
#include "lbard.h"

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
};

#define MAX_HF_STATIONS 1024
struct hf_station hf_stations[MAX_HF_STATIONS];
int hf_station_count=0;

int has_hf_plan=0;

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
  int minutes,hours;

  line[0]=0; fgets(line,1024,f);
  while(line[0]) {
    if ((line[0]=='#')||(line[0]<' ')) {
      // ignore blank lines and # comments
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
    if ((hf_station_count>0)&&(time(0)>=hf_next_call_time)) {
      int next_station = hf_next_station_to_call();
      if (next_station>-1) {
	snprintf(cmd,1024,"alecall %s \"!SERVAL,1,0\"\r\n",
		 hf_stations[next_station].name);
	write(serialfd,cmd,strlen(cmd));
	fprintf(stderr,"HF: Attempting to call station #%d '%s'\n",
		next_station,hf_stations[next_station].name);
	hf_link_partner=next_station;
	hf_state = HF_CALLREQUESTED|HF_COMMANDISSUED;
      }
    }
    break;
  case HF_CALLREQUESTED:
    break;
  case HF_CONNECTING:
    break;
  case HF_ALELINK:
    break;
  case HF_DISCONNECTING:
    break;
  default:
    break;
  }
  
  return 0;
}

int hf_codan_process_line(char *l)
{
  fprintf(stderr,"Codan radio (state 0x%04x) says: %s\n",hf_state,l);
  if (hf_state&HF_COMMANDISSUED) {
    // Ignore echoed commands, and wait for ">" prompt
    if (l[0]=='>') hf_state&=~HF_COMMANDISSUED;
    else if (!strcmp(l,"CALL STARTED")) {
      hf_state=HF_COMMANDISSUED|HF_CONNECTING;
    }
  }
  return 0;
}

int hf_barrett_process_line(char *l)
{
  fprintf(stderr,"Barrett radio says: %s\n",l);
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

  if (hf_state!=HF_ALELINK) return -1;
  
  fprintf(stderr,"Sending message of %d bytes via Codan HF\n",len);
  for(i=0;i<len;i+=66) {

    fragment[0]=0x30+(message_sequence_number&0x1f);
    fragment[1]=0x30+(i/66);
    // XXX - Add other bits here
    
    snprintf(message,8192,"amd %s\r\n",fragment);
    write_all(serialfd,message,strlen(message));
    // XXX - Wait for radio to respond
  }
  
  return -1;
}

int radio_send_message_barretthf(int serialfd,unsigned char *out, int len)
{
  return -1;
}