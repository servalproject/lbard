#include <strings.h>

#include "fakecsmaradio.h"

int rfdlora_read_byte(int i,unsigned char c)
{
  return 0;
}

int rfdlora_heartbeat(int client)
{
  return 0;
}

int rfdlora_encapsulate_packet(int from,int to,
				 unsigned char *packet,
				 int *packet_len)
{
  return 0;
}
