#include "fakecsmaradio.h"


int hfcodan_read_byte(int i,unsigned char c)
{
  if (c==0x15) {
    // Control-U -- clear input buffer
    clients[i].buffer_count=0;
  } else if (c!='\n'&&c!='\r'&&c) {
    // fprintf(stderr,"Radio #%d received character 0x%02x\n",i,c);    
    if (clients[i].buffer_count<(CLIENT_BUFFER_SIZE-1))
      clients[i].buffer[clients[i].buffer_count++]=c;
  } else {
    if (clients[i].buffer_count) {
      clients[i].buffer[clients[i].buffer_count]=0;
      fprintf(stderr,"Radio #%d sent command '%s'\n",i,clients[i].buffer);

      // Process the command here

      // Reset buffer ready for next command
      clients[i].buffer_count=0;
    }    
  }

  return 0;
}
