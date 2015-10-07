#include <stdio.h>
#include <time.h>
#include "lbard.h"

int energy_experiment(char *port, int pulse_frequency,float pulse_width_ms,
		      int wifiup_hold_time_ms)
{
  fprintf(stderr,"Running energy sample experiment:\n");
  fprintf(stderr,"  pulse width = %.1fms\n",pulse_width_ms);
  fprintf(stderr,"  pulse frequency = %dHz\n",pulse_frequency);
  fprintf(stderr,"  wifi hold time = %dms\n",wifiup_hold_time_ms);


  return 0;
}
