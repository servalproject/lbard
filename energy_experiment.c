#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#ifdef linux
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#endif

#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>

#include "sync.h"
#include "lbard.h"
#include "serial.h"

// Don't probe multimeter too often, or it will give invalid results from time to time
#define MEASURE_INTERVAL 100000

int multimeterfd=-1;

struct experiment_data {
  long long packet_number;
  int packet_len;
  int gap_us;
  int pulse_width_us;
  int pulse_frequency;
  int wifiup_hold_time_us;
  unsigned int key;

  // Used on the slave side only, but included in the packets for convenience
  double cycle_duration;
  double duty_cycle;
  int speed;
};

int pulse_widths[]={43,86,173,260,520,1041,2083,4166,8333,33333,0};





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

static int wifi_disable()
{
#ifdef linux
  fprintf(stderr,"Disabling wifi interface %s @ %lldms\n",
	  wifi_interface_name,gettime_ms());
  if (wifi_fd==-1)
    wifi_fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, wifi_interface_name);
  if (ioctl(wifi_fd,SIOCGIFFLAGS,&ifr)) {
    perror("SIOCGIFFLAGS failed");
    return -1;
  }
  ifr.ifr_flags&=!IFF_UP;
  if (ioctl(wifi_fd,SIOCSIFFLAGS,&ifr)) {
    perror("SIOCSIFFLAGS failed");
    return -1;
  }
#else
  fprintf(stderr,"wifi_disable() not implemented for this platform.\n");
  return -1;
#endif
  return 0;
}

static int wifi_enable()
{
#ifdef linux
  fprintf(stderr,"Enabling wifi interface %s @ %lldms\n",
	  wifi_interface_name,gettime_ms());
 if (wifi_fd==-1)
   wifi_fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
 struct ifreq ifr;
 memset(&ifr, 0, sizeof(ifr));
 strcpy(ifr.ifr_name, wifi_interface_name);
 if (ioctl(wifi_fd,SIOCGIFFLAGS,&ifr)) {
   perror("SIOCGIFFLAGS failed");
   return -1;
 }
 ifr.ifr_flags|=IFF_UP;
 if (ioctl(wifi_fd,SIOCSIFFLAGS,&ifr)) {
    perror("SIOCSIFFLAGS failed");
   return -1;
 }
#else
  fprintf(stderr,"wifi_disable() not implemented for this platform.\n");
  return -1;
#endif 
  return 0;
}


int setup_experiment(struct experiment_data *exp)
{
  exp->cycle_duration = 1000.0/exp->pulse_frequency;
  exp->duty_cycle=exp->pulse_width_us/10.0/exp->cycle_duration;

  if (0) {
    fprintf(stderr,"Running energy sample experiment:\n");
    fprintf(stderr,"  pulse width = %.4fms\n",exp->pulse_width_us/1000.0);
    fprintf(stderr,"  pulse frequency = %dHz\n",exp->pulse_frequency);
    fprintf(stderr,"  cycle duration (1/freq) = %3.2fms",exp->cycle_duration);
    fprintf(stderr,"  duty cycle = %3.2f%%\n",exp->duty_cycle);
    fprintf(stderr,"  wifi hold time = %.1fms\n",exp->wifiup_hold_time_us/1000.0);
  }

  if (exp->duty_cycle>99) {
    fprintf(stderr,"ERROR: Duty cycle cannot exceed 99%%\n");
    exit(-1);
  }

  if (exp->duty_cycle>90) {
    fprintf(stderr,"WARNING: Duty cycle is close to 100%% -- accuracy may suffer.\n");
  }
    
  // Work out correct serial port speed to produce the required pulse width
  int speed=-1;
  int possible_speeds[]={230400,115200,57600,38400,19200,9600,4800,2400,1200,300,0};
  int s;
  for(s=0;possible_speeds[s];s++) {
    // Pulse width will be 10 serial ticks wide for the complete character.
    int this_pulse_width=1000000*10/possible_speeds[s];
    if (((this_pulse_width-exp->pulse_width_us)<10)&&
	((this_pulse_width-exp->pulse_width_us)>-10))
      {
	speed=possible_speeds[s];
	break;
      }
  }
  if (speed==-1) {
    fprintf(stderr,
	    "Could not find a speed setting for pulse width of %.4fms (%dusec).\n",
	    exp->pulse_width_us/1000.0,exp->pulse_width_us);
    fprintf(stderr,"Possible pulse widths are:\n");
    for(s=0;possible_speeds[s];s++) {
      int this_pulse_width=1000000*10/possible_speeds[s];
      fprintf(stderr,"  %.4fms (%dusec)\n",
	      this_pulse_width/1000.0,
	      this_pulse_width);
    }
    return -1;
  }

  if (0) fprintf(stderr,"  serial port speed = %d\n",exp->speed);

  
  exp->speed=speed;
  return 0;
}

unsigned char wifi_activity_bitmap[1000];
time_t last_wifi_report_time=0;

int clear_wifi_activity_history()
{
  int i;
  for(i=0;i<1000;i++) wifi_activity_bitmap[i]=0;
  return 0;
}

int energy_experiment(char *port, char *interface_name)
{
  int sock=socket(AF_INET, SOCK_DGRAM, 0);
  if (sock==-1) {
    perror("Could not create UDP socket");
    exit(-1);
  }
  
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(19002);
  bind(sock, (struct sockaddr *) &addr, sizeof(addr));
  set_nonblock(sock);

  wifi_interface_name=interface_name;

  struct experiment_data exp;

  int experiment_invalid=1;
  
  int serialfd=-1;
  serialfd = open(port,O_RDWR);
  if (serialfd<0) {
    perror("Opening serial port");
    exit(-1);
  }
  fprintf(stderr,"Serial port open as fd %d\n",serialfd);

  // Start with reasonable interval, so that we can receive UDP from master in
  // reasonable time.
  int pulse_interval_usec=100000;
  
  // Start with wifi down
  int wifi_down=1;
  wifi_disable();
  long long wifi_down_time=0;
  
  int missed_pulses=0,sent_pulses=0;
  long long next_time=gettime_us();
  long long report_time=gettime_us()+1000;
  char nul[1]={0};
  int last_speed=-1;

  clear_wifi_activity_history();

  struct sockaddr_storage src_addr;
  socklen_t src_addr_len=sizeof(src_addr);
  unsigned char rx[9000];
  int r;
  
  while(1) {
    {
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
      
      while((r=recvfrom(sock,rx,9000,0,(struct sockaddr *)&src_addr,&src_addr_len))>0) {
	fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
      
	// Reflect packet back to sender
	sendto(sock,rx,r,0,(struct sockaddr *)&src_addr,src_addr_len);
	
	struct experiment_data *pd=(void *)&rx[0];
	if (0) printf("Saw packet with key 0x%08x : Updating experimental settings\n",
		      pd->key);
	exp=*pd;
	experiment_invalid=setup_experiment(&exp);
	if (!experiment_invalid)
	  if (last_speed!=exp.speed) {
	    fprintf(stderr,"last_speed=%d, exp.speed=%d\n",
		    last_speed,exp.speed);
	    if (serial_setup_port_with_speed(serialfd,exp.speed))
	      {
		fprintf(stderr,"Failed to setup serial port. Exiting.\n");
		exit(-1);
	      }
	    last_speed=exp.speed;
	  }
	pulse_interval_usec=1000000.0/exp.pulse_frequency;
	if (0) fprintf(stderr,"Sending a pulse every %dusec to achieve %dHz\n",
		       pulse_interval_usec,exp.pulse_frequency);
	
      }
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
      
      long long now=gettime_us();
      if (now>report_time) {
	report_time+=1000000;
	if ((sent_pulses != exp.pulse_frequency)||missed_pulses)
	  fprintf(stderr,"Sent %d pulses in the past second, and missed %d deadlines (target is %d).\n",
		  sent_pulses,missed_pulses,exp.pulse_frequency);
	sent_pulses=0;
	missed_pulses=0;
      }
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
      if (time(0)!=last_wifi_report_time) {
	last_wifi_report_time=time(0);
      	int i;
	printf("Wifi activity report @ %s",ctime(&last_wifi_report_time));
	for(i=0;i<1000;i++) {
	  printf("%c",wifi_activity_bitmap[i]);
	  if ((i%80)==79) printf("\n");
	}
	clear_wifi_activity_history();
      }
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
      
      if (now>=next_time) {
	// Next pulse is due, so write a single character of 0x00 to the serial port so
	// that the TX line is held low for 10 serial ticks (or should the byte be 0xff?)
	// which will cause the energy sampler to be powered for that period of time.
	write(serialfd, nul, 1);
	sent_pulses++;
	wifi_activity_bitmap[(now/1000)%1000]|='T';
	// Work out next time to send a character to turn on the energy sampler.
	// Don't worry about pulses that we can't send because we lost time somewhere,
	// just keep track of how many so that we can report this to the user.
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
	next_time+=pulse_interval_usec;
	while(next_time<now) {
	  next_time+=pulse_interval_usec;
	  missed_pulses++;
	}
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
      } else {
	// Wait for a little while if we have a while before the next time we need
	// to send a character. But busy wait the last 10usec, so that it doesn't matter
	// if we get woken up fractionally late.
	// Watcharachai will need to use an oscilliscope to see how adequate this is.
	// If there is too much jitter, then we will need to get more sophisticated.
	long long delay=next_time-now-10;
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
	if (delay>100000) delay=100000;
	if (delay>10) usleep(delay);
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
      }
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
      char buf[1024];
      set_nonblock(serialfd);
      ssize_t bytes = read_nonblock(serialfd, buf, sizeof buf);
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
      if (bytes>0) {
	wifi_activity_bitmap[(now/1000)%1000]|='R';
	// Work out when to take wifi low
	wifi_down_time=gettime_us()+exp.wifiup_hold_time_us;
	fprintf(stderr,"Saw energy on channel @ %lldms, holding Wi-Fi for %lld more usec\n",
		gettime_ms(),wifi_down_time-gettime_us());
	if (wifi_down) { wifi_enable(); wifi_down=0; }
      } else {
	if (now>wifi_down_time) {
	  if (wifi_down==0) wifi_disable();
	  wifi_down=1;
	}
      }
      fprintf(stderr,"%s:%d:Checkpoint\n",__FILE__,__LINE__);
    }   
  }
  return 0;
}

long long packet_number=0;
int build_packet(unsigned char *packet,
		 int gap_us,int packet_len,int pulse_width_us,
		 int pulse_frequency,int wifiup_hold_time_us,
		 int key)
{

  // Make packet empty
  bzero(packet,packet_len);

  // Copy packet information
  struct experiment_data p;
  p.packet_number=++packet_number;
  p.packet_len=packet_len;
  p.gap_us=gap_us;
  p.pulse_width_us=pulse_width_us;
  p.pulse_frequency=pulse_frequency;
  p.wifiup_hold_time_us=wifiup_hold_time_us;
  p.key=key;
  bcopy(&p,packet,sizeof(p));
  
  return 0;
}

int send_packet(int sock,unsigned char *packet,int len, char *broadcast_address)
{
  struct sockaddr_in addr;
  bzero(&addr, sizeof(addr)); 
  addr.sin_family = AF_INET; 
  addr.sin_port = htons(19002);
  addr.sin_addr.s_addr = inet_addr(broadcast_address);

  sendto(sock,packet,len,
	 MSG_DONTROUTE|MSG_DONTWAIT
#ifdef MSG_NOSIGNAL
	 |MSG_NOSIGNAL
#endif	       
	 ,(const struct sockaddr *)&addr,sizeof(addr));
  return 0;
}

float accumulated_current=0;
int current_samples=0;

unsigned char multimeter_message[1024];
int mmm_len=0;

int process_multimeter_line(unsigned char *msg)
{
  int plus_count=0;
  int i;
  for(i=0;msg[i];i++) if (msg[i]=='+') plus_count++;
  if (plus_count>1) {
    fprintf(stderr,"Ignoring malformed multimeter report '%s'\n",(char *)msg);
    return 0;
  }
    
  
  //  fprintf(stderr,"Multimeter says: %s\n",msg);
  if (!strcasecmp((char *)msg,"=>")) {
    //    fprintf(stderr,"Multimeter accepted a command.\n");
    return 0;
  }

  // Check for valid reading.  It reports 1 billion amps sometimes, perhaps when
  // we have asked for the result too frequenty.
  float m=0;
  int ofs;
  int n;
  if ((n=sscanf((char *)msg,"%f%n",&m,&ofs))==1) {
    if ((m<10)&&(m>=0.0000001)) {
      //      fprintf(stderr,"Read %lfA from multimeter (as %s)\n",m,msg);
      accumulated_current+=m;
      current_samples++;
    }
  }
  return 0;
}

int parse_multimeter_byte(unsigned char c)
{
  if ((c=='\r')||(c=='\n')) {
    multimeter_message[mmm_len]=0;
    if (mmm_len) process_multimeter_line(multimeter_message);
    mmm_len=0;
    return 0;
  }
  if (mmm_len<1024) {
    multimeter_message[mmm_len++]=c;
    return 0;
  }
  return -1;
}

int parse_multimeter_bytes(unsigned char *buff,int count)
{
  if (count<1) return 0;

  int i;
  for(i=0;i<count;i++) parse_multimeter_byte(buff[i]);
  return 0;
}

int run_energy_experiment(int sock,
			  int gap_us,int packet_len,int pulse_width_us,
			  int pulse_frequency,int wifiup_hold_time_us,
			  char *broadcast_address)
{
  // First, send a chain of packets until we get a reply acknowledging that
  // the required mode has been selected.
  unsigned char packet[1500];

  unsigned int key=random()&0xffffff;
  unsigned int key2=key|0xff000000;

  build_packet(packet,gap_us,packet_len,pulse_width_us,
	       pulse_frequency,wifiup_hold_time_us,
	       key);

  // Try for several seconds to get peers attention, sending lots of packets in short
  // intervals to try to get the remote side to wake up at least once.
  time_t timeout=time(0)+10;
  int peer_in_sync=0;
  while(time(0)<timeout) {
    send_packet(sock,packet,packet_len,broadcast_address);

    unsigned char rx[9000];
    int r=recvfrom(sock,rx,9000,0,NULL,0);
    if (r>0) {
      struct experiment_data *pd=(void *)&rx[0];
      // fprintf(stderr,"Saw packet with key 0x%08x, confirming sync\n",pd->key);
      if (pd->key==key) peer_in_sync=1;
    }
    if (peer_in_sync) break; else usleep(random()%10000);
  }
  if (!peer_in_sync) {
    fprintf(stderr,"Failed to gain peers attention within 10 seconds.\n");
    printf("%d:%d:%d:%d:%d:-1:0:0:-1:-1\n",gap_us,packet_len,pulse_width_us,
	   pulse_frequency,wifiup_hold_time_us);
    return 0;
  }

  // Wait for wifi to shut down on the remote side.
  usleep(wifiup_hold_time_us);  // time required
  usleep(1000000); // insurance of extra 1 second
  // Clear out any banked up packets
  unsigned char rx[9000];
  int queue=0;
  while (recvfrom(sock,rx,9000,0,NULL,0)>0) queue++;
  // printf("Cleared %d queued packets.\n",queue);
  
  // No run the experiment 20 times

  int received_replies_to_first_packets=0;
  int received_replies_to_second_packets=0;

  // Wait for wifi to shut down on the remote side.
  usleep(wifiup_hold_time_us);  // time required
  usleep(1000000); // insurance of extra 1 second

  // Clear any pending input from the multimeter
  unsigned char buff[8192];
  int r=read(multimeterfd,buff,8192);

  // Measure current over a couple of seconds to get a clear idea of the current
  // consumption in this mode
  fprintf(stderr,"Calculating average current draw with wi-fi off...\n");
  long long collect_timeout=gettime_us()+10000000;
  long long next_measurement=0;
  
  accumulated_current=0;
  current_samples=0;
  while(current_samples<10) {
    collect_timeout=gettime_us()+10000000;
    current_samples=0;
    while(gettime_us()<collect_timeout) {
      // Poll for current draw every 20ms
      if (gettime_us()>=next_measurement) {
	//	fprintf(stderr,"Asking multimeter for instantaneous current reading.\n");
	write(multimeterfd,"VAL?\r\n",6);
	next_measurement=gettime_us()+MEASURE_INTERVAL;
      }
      r=read(multimeterfd,buff,8192);
      if (r>0) {
	// Look for multimeter current reading reports
	parse_multimeter_bytes(buff,r);
      }
      
      // Sleep for a short while between events.
      usleep(500);
    }
  }
  float average_current=-1;
  int average_current_samples=current_samples;
  if (current_samples) {
    average_current=accumulated_current/current_samples;
    fprintf(stderr,"Average current draw while wifi off is %fA (from %d samples)\n",
	    average_current,current_samples);
  }

  
  int iteration;
  for(iteration=0;iteration<20;iteration++) {
    // printf("Iteration #%d\n",iteration);
    
    // Wait for wifi to shut down on the remote side.
    usleep(wifiup_hold_time_us);  // time required
    usleep(1000000); // insurance of extra 1 second

    // Now flush multimeter serial input
    unsigned char buff[8192];
    int r=read(multimeterfd,buff,8192);
    
    // Then send a packet
    build_packet(packet,gap_us,packet_len,pulse_width_us,
		 pulse_frequency,wifiup_hold_time_us,
		 key2);
    long long first_id=packet_number;
    send_packet(sock,packet,packet_len,broadcast_address);
    if (0) printf("Sent packet with key 0x%08x, id = %lld, then waiting %dusec before sending duplicate\n",
	   key,packet_number,gap_us);

    // Maasure current draw of experiment while waiting for gap timeout
    //usleep(gap_us);
    long long gap_timeout=gettime_us()+gap_us;
    next_measurement=0;
    while(gettime_us()<gap_timeout) {
      // Poll for current draw every 20ms
      if (gettime_us()>=next_measurement) {
	//	fprintf(stderr,"Asking multimeter for instantaneous current reading.\n");
	write(multimeterfd,"VAL?\r\n",6);
	next_measurement=gettime_us()+MEASURE_INTERVAL;
      }
      r=read(multimeterfd,buff,8192);
      if (r>0) {
	// Look for multimeter current reading reports
	parse_multimeter_bytes(buff,r);
      }

      // Sleep for a short while between events.
      usleep(500);
    }

    r=read(multimeterfd,buff,8192);
    if (r>0) {
      // Look for multimeter current reading reports
      parse_multimeter_bytes(buff,r);
    }
    
    build_packet(packet,gap_us,packet_len,pulse_width_us,
		 pulse_frequency,wifiup_hold_time_us,
		 key2);
    long long second_id=packet_number;
    send_packet(sock,packet,packet_len,broadcast_address);
    
    timeout=time(0)+3;
    while(time(0)<timeout) {
      int r=recvfrom(sock,rx,9000,0,NULL,0);
      if (r>0) {
	struct experiment_data *pd=(void *)&rx[0];
	if (0)
	  printf("Saw candidate reply, key=0x%08x, packet_number=%lld (expecting %lld or %lld)\n",
		 pd->key,pd->packet_number,first_id,second_id);
	if (pd->key==key2) {
	  if (pd->packet_number==second_id) {
	    received_replies_to_second_packets++;
	    if (0) printf("  received reply to data packet.\n");
	    break;
	  } 
	  if (pd->packet_number==first_id) {
	    received_replies_to_first_packets++;
	    if (0) printf("  received reply to wake packet.\n");
	  } 
	}
      }

      // Poll for current draw every 20ms
      if (gettime_us()>=next_measurement) {
	//	fprintf(stderr,"Asking multimeter for instantaneous current reading.\n");
	write(multimeterfd,"VAL?\r\n",6);
	next_measurement=gettime_us()+MEASURE_INTERVAL;
      }
      r=read(multimeterfd,buff,8192);
      if (r>0) {
	// Look for multimeter current reading reports
	parse_multimeter_bytes(buff,r);
      }
      
    }
  }
  printf("%d:%d:%d:%d:%d:20:%d:%d:%f:%d\n",gap_us,packet_len,pulse_width_us,
	 pulse_frequency,wifiup_hold_time_us,
	 received_replies_to_second_packets,
	 received_replies_to_first_packets,
	 average_current,average_current_samples);
  

  return 0;
}

int energy_experiment_master(char *port,char *broadcast_address)
{
  // Setup serial port to speak to digital multi-meter to get current readings
  // during the tests.
  multimeterfd = open(port,O_RDWR);
  if (multimeterfd<0) {
    perror("Opening serial port");
    exit(-1);
  }
  fprintf(stderr,"Multi-meter serial port open as fd %d\n",multimeterfd);
  serial_setup_port_with_speed(multimeterfd,9600);
  // Display measurement without exponent, because the multimeter sometimes reports
  // the wrong exponent.
  char *setup[]={"\r\n",
		 "ADC\r\n",
		 "FILTER 0\r\n",
		 "RANGE 1\r\n",
		 "FORMAT 3\r\n",
		 NULL};
  int i;
  for(i=0;setup[i];i++) {
    usleep(200000);
    write(multimeterfd,setup[i],strlen(setup[i]));
  }
  unsigned char buff[8192];
  int r=read(multimeterfd,buff,8192);
  parse_multimeter_bytes(buff,r);

  int sock=socket(AF_INET, SOCK_DGRAM, 0);
  if (sock==-1) {
    perror("Could not create UDP socket");
    exit(-1);
  }
  
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(19001);
  bind(sock, (struct sockaddr *) &addr, sizeof(addr));
  set_nonblock(sock);
  
  // Enable broadcast
  int one=1;
  r=setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  if (r) {
    fprintf(stderr,"WARNING: setsockopt(): Could not enable SO_BROADCAST\n");
  }

  /*
    For this experiment we vary the gap between sending pairs of packets,
    and keep track of the percentage of replies in each case.  The client side
    will be using the energy sampler program to control its wifi interface.
    We will also send it the parameters it should be using.
  */
  int gap_us;
  int packet_len;
  int pulse_width_us;
  int pulse_frequency;
  int wifiup_hold_time_us;

  long long experiment_count=0;

  int gaps[]={1000,3000,5000,10000,50000,100000,500000,1000000,0};
  int gap_number;

  int pulse_frequencies[]={10,50,100,500,1000,0};
  int freq_number;

  int wifiup_hold_times[]={5000,50000,100000,500000,1000000,3000000,0};
  int hold_number;

  
  for(gap_number=0;gaps[gap_number];gap_number++) {
    gap_us=gaps[gap_number];
    for(packet_len=100;packet_len<=1500;packet_len+=300) {
      int pulse_width_index=0;
      for(pulse_width_us=pulse_widths[pulse_width_index];
	  pulse_widths[pulse_width_index];pulse_width_index++) {
	for(freq_number=0;pulse_frequencies[freq_number]
	      &&(pulse_frequencies[freq_number]*pulse_width_us)<=900000;
	    freq_number++) {	  
	  pulse_frequency=pulse_frequencies[freq_number];
	  for(hold_number=0;wifiup_hold_times[hold_number];
	      hold_number++)
	    if (wifiup_hold_times[hold_number]>=gap_us) {
	    wifiup_hold_time_us=wifiup_hold_times[hold_number];
	    experiment_count++;
	  }
	}
      }
    }
  }
  fprintf(stderr,"Preparing to run %lld experiments.\n",
	  experiment_count);

  long long experiment_number=0;
  time_t start_time=time(0);

  for(gap_number=0;gaps[gap_number];gap_number++) {
    gap_us=gaps[gap_number];
    for(packet_len=100;packet_len<=1500;packet_len+=300) {
      int pulse_width_index=0;
      for(pulse_width_us=pulse_widths[pulse_width_index];
	  pulse_widths[pulse_width_index];pulse_width_index++) {
	for(freq_number=0;pulse_frequencies[freq_number]
	      &&(pulse_frequencies[freq_number]*pulse_width_us)<=900000;
	    freq_number++) {	  
	  pulse_frequency=pulse_frequencies[freq_number];
	  for(hold_number=0;wifiup_hold_times[hold_number];
	      hold_number++)
	    if (wifiup_hold_times[hold_number]>=gap_us) {
	    wifiup_hold_time_us=wifiup_hold_times[hold_number];
	    experiment_number++;
	    fprintf(stderr,"T+%ld : Experiment %lld/%lld (%2.1f%%)\n",
		    time(0)-start_time,
		    experiment_number,experiment_count,
		    experiment_number*100.0/experiment_count);
	    run_energy_experiment(sock,
				  gap_us,packet_len,pulse_width_us,
				  pulse_frequency,wifiup_hold_time_us,
				  broadcast_address);
	  }
	}
      }
    }
  }
  return 0;
}
