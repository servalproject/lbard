#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include "lbard.h"

int energy_experiment(char *port, int pulse_frequency,float pulse_width_ms,
		      int wifiup_hold_time_ms)
{
  fprintf(stderr,"Running energy sample experiment:\n");
  fprintf(stderr,"  pulse width = %.4fms\n",pulse_width_ms);
  fprintf(stderr,"  pulse frequency = %dHz\n",pulse_frequency);
  fprintf(stderr,"  wifi hold time = %dms\n",wifiup_hold_time_ms);

  // Work out correct serial port speed to produce the required pulse width
  int speed=-1;
  int pulse_width_usec=pulse_width_ms*1000;
  int possible_speeds[]={230400,115200,57600,38400,19200,9600,4800,2400,1200,300,0};
  int s;
  for(s=0;possible_speeds[s];s++) {
    int this_pulse_width=1000000*10/possible_speeds[s];
    if (((this_pulse_width-pulse_width_usec)<10)&&
	((this_pulse_width-pulse_width_usec)>-10))
      {
	speed=possible_speeds[s];
	break;
      }
  }
  if (speed==-1) {
    fprintf(stderr,
	    "Could not find a speed setting for pulse width of %.4fms (%dusec).\n",
	    pulse_width_ms,pulse_width_usec);
    fprintf(stderr,"Possible pulse widths are:\n");
    for(s=0;possible_speeds[s];s++) {
      int this_pulse_width=1000000*10/possible_speeds[s];
      fprintf(stderr,"  %.4fms (%dusec)\n",
	      this_pulse_width/1000.0,
	      this_pulse_width);
    }
    exit(-1);
  }

  int serialfd=-1;
  serialfd = open(port,O_RDWR);
  if (serialfd<0) {
    perror("Opening serial port");
    exit(-1);
  }
  if (serial_setup_port_with_speed(serialfd,speed))
    {
      fprintf(stderr,"Failed to setup serial port. Exiting.\n");
      exit(-1);
    }
  fprintf(stderr,"Serial port open as fd %d\n",serialfd);

  int pulse_interval_usec=1000000.0/pulse_frequency;
  fprintf(stderr,"Sending a pulse every %dusec to achieve %dHz\n",
	  pulse_interval_usec,pulse_frequency);

  
  
  return 0;
}
