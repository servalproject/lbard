#include <strings.h>

#include "fakecsmaradio.h"

int last_was_cr=0;
int rfdlora_read_byte(int i,unsigned char c)
{
  if ((c=='\n')&&last_was_cr) {
    // Have line of input 
        if (clients[i].buffer_count) {
      
      // trim CR from end
      clients[i].buffer[clients[i].buffer_count-1]=0;
      
      fprintf(stderr,"RFD LoRa Radio #%d was sent command '%s'\n",i,clients[i].buffer);

      // Process the command here
      if (!strcasecmp("mac pause",(char *)clients[i].buffer)) {
        // Stop MAC from receiving (we will ignore any packets that come, until we resume it
      } else if (!strcasecmp("radio rx 0",(char *)clients[i].buffer)) {
        // Resume receiving packets
      } else if (!strcasecmp("sys get pindig GPIO13",(char *)clients[i].buffer)) {
        // 
      } else if (!strcasecmp("sys set pindig GPIO13 1",(char *)clients[i].buffer)) {
        // 
      } else if (!strcasecmp("sys set pindig GPIO13 0",(char *)clients[i].buffer)) {
        // 
      } else if (!strcasecmp("sys reset",(char *)clients[i].buffer)) {
        write(clients[i].socket,"RN2903 1.0.3 Aug  8 2017 15:11:09\r\n",strlen("RN2903 1.0.3 Aug  8 2017 15:11:09\r\n"));
      } else if (!strcasecmp("",(char *)clients[i].buffer)) {
        // 
      } else {
        write(clients[i].socket,"invalid param\r\n",strlen("invalid param\r\n"));
      }
      // Reset buffer ready for next command
      clients[i].buffer_count=0;
    }    

  } else {
    // fprintf(stderr,"Radio #%d received character 0x%02x\n",i,c);
   
    if (clients[i].buffer_count<(CLIENT_BUFFER_SIZE-1))
      clients[i].buffer[clients[i].buffer_count++]=c;
  }
   if (c=='\r') last_was_cr=1; else last_was_cr=0;
 
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
