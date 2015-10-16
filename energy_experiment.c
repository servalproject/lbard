#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "lbard.h"
#ifdef linux
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#endif

// From os.c in serval-dna
long long gettime_us()
{
  struct timeval nowtv;
  // If gettimeofday() fails or returns an invalid value, all else is lost!
  if (gettimeofday(&nowtv, NULL) == -1)
    return -1;
  if (nowtv.tv_sec < 0 || nowtv.tv_usec < 0 || nowtv.tv_usec >= 1000000)
    return -1;
  return nowtv.tv_sec * 1000000LL + nowtv.tv_usec;
}

char *wifi_interface_name=NULL;
int wifi_fd=-1;

#else
  return -1;
#endif
}

int wifi_enable()
{
#ifdef linux
  fprintf(stderr,"Enabling wifi interface %s @ %lldms\n",
	  wifi_interface_name,gettime_ms());
 if (wifi_fd==-1)
   wifi_fd = socket(PF_INET6, SOCK_DGRAM, IPPROTO_IP);
 struct ifreq ifr;
 memset(&ifr, 0, sizeof(ifr));
 strcpy(ifr.ifr_name, wifi_interface_name);
 if (ioctl(wifi_fd,SIOCGIFFLAGS,&ifr)) return -1;
 ifr.ifr_flags|=IFF_UP;
 if (ioctl(wifi_fd,SIOCSIFFLAGS,&ifr)) return -1;
#else
  return -1;
#endif 
}


int energy_experiment(char *port, int pulse_frequency,float pulse_width_ms,
		      int wifiup_hold_time_ms, char *interface_name)
{
  fprintf(stderr,"Running energy sample experiment:\n");
  fprintf(stderr,"  pulse width = %.4fms\n",pulse_width_ms);
  fprintf(stderr,"  pulse frequency = %dHz\n",pulse_frequency);
  fprintf(stderr,"  wifi hold time = %dms\n",wifiup_hold_time_ms);
  fprintf(stderr,"  wifi interface = %s\n",interface_name);

  wifi_interface_name=interface_name;
  
  // Work out correct serial port speed to produce the required pulse width
  int speed=-1;
  int pulse_width_usec=pulse_width_ms*1000;
  int possible_speeds[]={230400,115200,57600,38400,19200,9600,4800,2400,1200,300,0};
  int s;
  for(s=0;possible_speeds[s];s++) {
    // Pulse width will be 10 serial ticks wide for the complete character.
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

  // Start with wifi down
  int wifi_down=1;
  wifi_disable();
  long long wifi_down_time=0;

  int missed_pulses=0,sent_pulses=0;
  long long next_time=gettime_us();
  long long report_time=gettime_us()+1000;
  char nul[1]={0};
  while(1) {
    long long now=gettime_us();
    if (now>report_time) {
      report_time+=1000000;
      fprintf(stderr,"Sent %d pulses in the past second, and missed %d deadlines (target is %d).\n",
	      sent_pulses,missed_pulses,pulse_frequency);
      sent_pulses=0;
      missed_pulses=0;
    }
    if (now>=next_time) {
      // Next pulse is due, so write a single character of 0x00 to the serial port so
      // that the TX line is held low for 10 serial ticks (or should the byte be 0xff?)
      // which will cause the energy sampler to be powered for that period of time.
      write(serialfd, nul, 1);
      sent_pulses++;
      // Work out next time to send a character to turn on the energy sampler.
      // Don't worry about pulses that we can't send because we lost time somewhere,
      // just keep track of how many so that we can report this to the user.
      next_time+=pulse_interval_usec;
      while(next_time<now) {
	next_time+=pulse_interval_usec;
	missed_pulses++;
      }
    } else {
      // Wait for a little while if we have a while before the next time we need
      // to send a character. But busy wait the last 10usec, so that it doesn't matter
      // if we get woken up fractionally late.
      // Watcharachai will need to use an oscilliscope to see how adequate this is.
      // If there is too much jitter, then we will need to get more sophisticated.
      if (next_time-now>10) usleep(next_time-now-10);
    }
    char buf[1024];
    ssize_t bytes = read_nonblock(serialfd, buf, sizeof buf);
    if (bytes>0) {
      // Work out when to take wifi low
      wifi_down_time=gettime_us()+wifiup_hold_time_ms*1000;
      if (wifi_down) { wifi_enable(); wifi_down=0; }
    } else {
      if (now>wifi_down_time) { wifi_disable(); wifi_down=1; }
    }
  }    
  
  return 0;
}
