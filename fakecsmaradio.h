#include <stdio.h>
#include <stdint.h>
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

int filter_and_enqueue_packet_for_client(int from,int to, long long delivery_time,
					 uint8_t *packet_in,int packet_len);
long long gettime_ms();
int rfd900_read_byte(int client,unsigned char byte);

#include "fec-3.0.1/fixed.h"
void encode_rs_8(data_t *data, data_t *parity,int pad);
int decode_rs_8(data_t *data, int *eras_pos, int no_eras, int pad);
#define FEC_LENGTH 32
#define FEC_MAX_BYTES 223

extern long long start_time;

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
extern struct client clients[MAX_CLIENTS];
extern int client_count;
