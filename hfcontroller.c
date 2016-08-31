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
    } else {
      fprintf(stderr,"Unknown directive in HF radio plan file.\n");
      fprintf(stderr,"  Offending line: %s\n",line);
      exit(-1);	
    }
    line[0]=0; fgets(line,1024,f);
  }
  fclose(f);

  has_hf_plan=1;
  
  return 0;
}

int hf_serviceloop(int serialfd)
{
  if (!has_hf_plan) {
    fprintf(stderr,"You must specify a HF radio plan via the hfplan= command line option.\n");
    exit(-1);
  }
  
  return 0;
}
