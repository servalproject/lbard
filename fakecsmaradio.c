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

	// Build envelope
	packet[packet_len++]=0xaa;
	packet[packet_len++]=0x55;
	packet[packet_len++]=200; // RSSI of this frame
	packet[packet_len++]=100; // Average RSSI remote side
	packet[packet_len++]=28; // Temperature of this radio
	packet[packet_len++]=send_bytes; // length of this packet
	packet[packet_len++]=0xff;  // 16-bit RX buffer space (always claim 4095 bytes)
	packet[packet_len++]=0x0f;
	packet[packet_len++]=0x55;	
	// Now packet body, upto 255 bytes
	bcopy(&clients[client].buffer[0],
	      &packet[packet_len],
	      send_bytes);
	packet_len+=send_bytes;
	bcopy(&clients[client].buffer[send_bytes],
	      &clients[client].buffer[0],
	      clients[client].buffer_count-send_bytes);
	clients[client].buffer_count-=send_bytes;

	// Work out when the packet should be delivered
	// (include 8 bytes time for the preamble)
	int transmission_time = 1000000*8*(8+send_bytes)/emulated_bitrate;
	long long delivery_time = gettime_ms()+transmission_time;
	
	// Queue bytes for RX by remote side.
	// Set delay according to length of packet and chosen bit rate.
	// Note that this approach means that colliding packets will cause them to
	// fail to be delivered, which is probably a good thing
	for(int j=0;j<client_count;j++) {
	  if (j!=client) {
	    bcopy(packet,clients[j].rx_queue,packet_len);
	    if (clients[j].rx_queue_len) {
	      printf("WARNING: RX colission for radio #%d\n",j);
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
  int sock=-1;
  struct sockaddr_un server_address;
  struct sockaddr_un client_address;

  // Create bare socket
  sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock<0) {
    perror("Could not create server socket.\n");
    exit(-1);
  }

  // Set socket name
  bzero(&server_address, sizeof(server_address));
  server_address.sun_family = AF_UNIX;
  sprintf(server_address.sun_path,"%s",socketname);

  // Bind name to socket
  unlink(socketname);
  if (bind(sock, (const struct sockaddr *) &server_address, sizeof(server_address))<0) {
    perror("Could not bind server socket");
    close(sock);
    exit(-1);
  }

  // Set server socket non-blocking
  set_nonblocking(sock);

  long long last_heartbeat_time=0;
  
  // look for new clients, and for traffic from each client.
  unsigned int client_addr_len = sizeof(client_address);
  while(1) {
    int activity=0;
    
    // Check for new connections
    int client_sock = accept(sock,(struct sockaddr *)&client_address,&client_addr_len);
    if (client_sock>-1) {
      fprintf(stderr,"New connection.\n");
      register_client(client_sock);
      activity++;
    }

    // Read from each client, and see if we have a packet to release
    long long now = gettime_ms();
    for(int i=0;i<client_count;i++) {
      unsigned char buffer[8192];
      size_t count = read(clients[i].socket,buffer,8192);
      if (count>0) {
	for(int j=0;j<count;j++) client_read_byte(i,buffer[j]);
	activity++;
      }

      // Release any queued packet once we pass the embargo time
      if (clients[i].rx_queue_len&&(clients[i].rx_embargo<now))
	{
	  if (!clients[i].rx_colission) {
	    write(clients[i].socket,
		  clients[i].rx_queue,
		  clients[i].rx_queue_len);
	  }
	  clients[i].rx_queue_len=0;
	  clients[i].rx_colission=0;
	  activity++;
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
