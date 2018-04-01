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
#include "hf.h"

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
