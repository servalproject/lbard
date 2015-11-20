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

char *socketname="/tmp/fakecsmaradio.socket";

struct client {
  int socket;

  int rx_state;
#define STATE_NORMAL 0
#define STATE_BANG 1

#define CLIENT_BUFFER_SIZE 4096
  unsigned char buffer[CLIENT_BUFFER_SIZE];
  int buffer_count;
};

#define MAX_CLIENTS 1024
struct client clients[MAX_CLIENTS];
int client_count=0;

// Emulate this bitrate on the radios
// (emulate some (but not all) collission modes for
// simultaneous transmission).
int emulated_bitrate = 128000;

int register_client(int client_socket)
{
  if (client_count>=MAX_CLIENTS) {
    fprintf(stderr,"Too many clients: Increase MAX_CLIENTS?\n");
    exit(-1);
  }

  bzero(&clients[client_count],sizeof(struct client));
  clients[client_count].socket = client_socket;
  client_count++;
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
  
  // look for new clients, and for traffic from each client.
  unsigned int client_addr_len = sizeof(client_address);
  while(1) {
    int client_sock = accept(sock,(struct sockaddr *)&client_address,&client_addr_len);
    if (client_sock>-1) {
      fprintf(stderr,"New connection.\n");
      register_client(client_sock);
    }
    
  }
  
}
