// Based oncode from:
// http://stackoverflow.com/questions/10359067/unix-domain-sockets-on-linux
// which in turn was sourced from:
// 

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

int packet_drop_threshold=0;

char *socketname="/tmp/fakecsmaradio.socket";

struct client {
  int socket;

  int rx_state;
#define STATE_NORMAL 0
#define STATE_BANG 1

#define CLIENT_BUFFER_SIZE 4096
  unsigned char buffer[CLIENT_BUFFER_SIZE];
  int buffer_count;

  // Buffer holding received packet ready for sending when transmission
  // time actually expires.
  unsigned char rx_queue[CLIENT_BUFFER_SIZE];
  int rx_queue_len;
  long long rx_embargo;
  int rx_colission;
  
};

#define MAX_CLIENTS 1024
struct client clients[MAX_CLIENTS];
int client_count=0;

// Emulate this bitrate on the radios
// (emulate some (but not all) collission modes for
// simultaneous transmission).
int emulated_bitrate = 128000;

long long start_time;

int set_nonblocking(int fd)
{
  fcntl(fd,F_SETFL,fcntl(fd, F_GETFL, NULL)|O_NONBLOCK);
  return 0;
}

long long gettime_ms()
{
  struct timeval nowtv;
  // If gettimeofday() fails or returns an invalid value, all else is lost!
  if (gettimeofday(&nowtv, NULL) == -1) return -1;

  return nowtv.tv_sec * 1000LL + nowtv.tv_usec / 1000;
}

int register_client(int client_socket)
{
  if (client_count>=MAX_CLIENTS) {
    fprintf(stderr,"Too many clients: Increase MAX_CLIENTS?\n");
    exit(-1);
  }

  bzero(&clients[client_count],sizeof(struct client));
  clients[client_count].socket = client_socket;
  client_count++;

  set_nonblocking(client_socket);
  
  return 0;
}

int dump_bytes(char *msg,unsigned char *bytes,int length)
{
  fprintf(stderr,"%s:\n",msg);
  for(int i=0;i<length;i+=16) {
    fprintf(stderr,"%04X: ",i);
    for(int j=0;j<16;j++) if (i+j<length) fprintf(stderr," %02X",bytes[i+j]);
    fprintf(stderr,"\n");
  }
  return 0;
}

int client_read_byte(int client,unsigned char byte)
{
  switch(clients[client].rx_state) {
  case STATE_BANG:
    clients[client].rx_state=STATE_NORMAL;
    switch(byte) {
    case '!': // TX now
      {
	unsigned char packet[10000];
	int packet_len=0;
	int send_bytes=clients[client].buffer_count;
	if (send_bytes>255) send_bytes=255;

	// First the packet body, upto 255 bytes
	bcopy(&clients[client].buffer[0],
	      &packet[packet_len],
	      send_bytes);
	packet_len+=send_bytes;
	bcopy(&clients[client].buffer[send_bytes],
	      &clients[client].buffer[0],
	      clients[client].buffer_count-send_bytes);
	clients[client].buffer_count-=send_bytes;
	
	// Then build and attach envelope
	packet[packet_len++]=0xaa;
	packet[packet_len++]=0x55;
	packet[packet_len++]=200; // RSSI of this frame
	packet[packet_len++]=100; // Average RSSI remote side
	packet[packet_len++]=28; // Temperature of this radio
	packet[packet_len++]=send_bytes; // length of this packet
	packet[packet_len++]=0xff;  // 16-bit RX buffer space (always claim 4095 bytes)
	packet[packet_len++]=0x0f;
	packet[packet_len++]=0x55;	

	// Work out when the packet should be delivered
	// (include 8 bytes time for the preamble)
	// Calculate first in usec, then divide down to ms
	int transmission_time = 1000000*8*(8+send_bytes)/emulated_bitrate;
	transmission_time/=1000;
	long long delivery_time = gettime_ms()+transmission_time;
	
	// Queue bytes for RX by remote side.
	// Set delay according to length of packet and chosen bit rate.
	// Note that this approach means that colliding packets will cause them to
	// fail to be delivered, which is probably a good thing
	printf("Radio #%d sends a packet of %d bytes at T+%lldms (TX will take %dms)\n",
	       client,packet_len,gettime_ms()-start_time,transmission_time);

	dump_bytes("packet",packet,packet_len);
	
	for(int j=0;j<client_count;j++) {
	  if (j!=client) {
	    bcopy(packet,clients[j].rx_queue,packet_len);
	    long long now=gettime_ms();
	    if (clients[j].rx_queue_len) {
	      printf("WARNING: RX colission for radio #%d (embargo time = T%+lldms, last packet = %d bytes)\n",
		     j,clients[j].rx_embargo-now,clients[j].rx_queue_len);
	      clients[j].rx_colission=1;
	    } else clients[j].rx_colission=0;
	    clients[j].rx_queue_len=packet_len;
	    clients[j].rx_embargo=delivery_time;
	  }
	}
      }
      break;
    case 'C':
      clients[client].buffer_count=0;
      break;
    case 'F': // Report flash version
      // Not required
      break;
    case 'H': // set TX power high
      // Not required
      printf("Setting radio #%d to high TX power\n",client);
      break;
    case 'L': // set TX power high
      // Not required
      printf("Setting radio #%d to low TX power\n",client);
      break;
    case 'R': // Reset radio paramegers
      // Not required
      break;
    case 'Z': // Reset radio
      clients[client].buffer_count=0;
      break;
    case 'V': // version
      write(clients[client].socket,"1",1);
      break;
    case '.': // escaped !
      if (clients[client].buffer_count<CLIENT_BUFFER_SIZE)
	clients[client].buffer[clients[client].buffer_count++]='!';
      break;
    default: // unknown escape
      write(clients[client].socket,"E",1);
      break;
    }
    
    break;
  case STATE_NORMAL:
    if (byte!='!') {
      if (clients[client].buffer_count<CLIENT_BUFFER_SIZE)
	clients[client].buffer[clients[client].buffer_count++]=byte;
    } else {
      clients[client].rx_state=STATE_BANG;
    }
    break;
  }

  return 0;
}

int main(int argc,char **argv)
{
  int radio_count=2;
  FILE *tty_file=NULL;

  start_time=gettime_ms();
  
  if (argv&&argv[1]) radio_count=atoi(argv[1]);
  if (argc>2) tty_file=fopen(argv[2],"w");
  if ((argc<3)||(argc>4)||(!tty_file)||(radio_count<2)||(radio_count>=MAX_CLIENTS)) {
    fprintf(stderr,"usage: fakecsmaradio <number of radios> <tty file> [packet drop probability]\n");
    fprintf(stderr,"\nNumber of radios must be between 2 and %d.\n",MAX_CLIENTS-1);
    fprintf(stderr,"The name of each tty will be written to <tty file>\n");
    fprintf(stderr,"The optional packet drop probability allows the simulation of packet loss.\n"); 
    exit(-1);
  }
  if (argc>3) 
    {
      float p=atof(argv[3]);
      if (p<0||p>1) {
	fprintf(stderr,"Packet drop probability must be in range [0..1]\n");
	exit(-1);
      }
      packet_drop_threshold = p*0x7fffffff;
      fprintf(stderr,"Simulating %3.2f%% packet loss (threshold = 0x%08x)\n",
	      p*100.0,packet_drop_threshold);
    }
  srandom(time(0));

  for(int i=0;i<radio_count;i++) {
    int fd=posix_openpt(O_RDWR|O_NOCTTY);
    if (fd<0) {
      perror("posix_openpt");
      exit(-1);
    }
    grantpt(fd);
    unlockpt(fd);
    fcntl(fd,F_SETFL,fcntl(fd, F_GETFL, NULL)|O_NONBLOCK);
    fprintf(tty_file,"%s\n",ptsname(fd));
    printf("Radio #%d is available at %s\n",client_count,ptsname(fd));
    clients[client_count++].socket=fd;       
  }
  fclose(tty_file);
  
  long long last_heartbeat_time=0;
  
  // look for new clients, and for traffic from each client.
  while(1) {
    int activity=0;
    
    // Read from each client, and see if we have a packet to release
    long long now = gettime_ms();
    for(int i=0;i<client_count;i++) {
      unsigned char buffer[8192];
      int count = read(clients[i].socket,buffer,8192);
      if (count>0) {
	for(int j=0;j<count;j++) client_read_byte(i,buffer[j]);
	activity++;
      }

      // Release any queued packet once we pass the embargo time
      if (clients[i].rx_queue_len&&(clients[i].rx_embargo<now))
	{
	  if (!clients[i].rx_colission) {
	    if ((random()&0x7fffffff)>=packet_drop_threshold) {
	      write(clients[i].socket,
		    clients[i].rx_queue,
		    clients[i].rx_queue_len);
	      printf("Radio #%d receives a packet of %d bytes\n",
		     i,clients[i].rx_queue_len);
	    } else
	      printf("Radio #%d misses a packet of %d bytes due to simulated packet loss\n",
		     i,clients[i].rx_queue_len);
	      
	  }
	  printf("Radio #%d ready to receive.\n",i);
	  clients[i].rx_queue_len=0;
	  clients[i].rx_colission=0;
	  activity++;
	} else {
	if (clients[i].rx_embargo&&clients[i].rx_queue_len)
	  printf("Radio #%d WAITING until T+%lldms for a packet of %d bytes\n",
		 i,clients[i].rx_embargo-now,clients[i].rx_queue_len);

      }
    }

    if (last_heartbeat_time<(now-500)) {
      // Pretend to be reporting GPIO status so that lbard thinks the radio is alive.
      unsigned char heartbeat[9]={0xce,0xec,0xff,0xff,0xff,0xff,0xff,0xff,0xdd};
      for(int i=0;i<client_count;i++) {
	write(clients[i].socket,heartbeat, sizeof(heartbeat));
      }
      last_heartbeat_time=now;
    }

    // Sleep for 10ms if there has been no activity, else look for more activity
    if (!activity) usleep(10000);      
  }
  
}
