#include <strings.h>

#include "fakecsmaradio.h"

struct barrett_radio_state {

  // These flags reflect the identically named Barrett RS-232 commands
  // that enable/disable various notification functions
  int aramdm;
  int aramdp;
  int arcall;
  int arlink;
  int arltbl;
  int armess;
  int arstat;

  // The set of channels

  // The list of radios 
  
// Commands and responses we implement
// "AXLINK'+<link partner> make connection to peer
//     (modem doesn't respond preemptively, must be queried with AILTBL)
// "AILTBL" - query current ALE link status
//   "AILTBL" - ALE not connected/no longer connected
//   "AILTBL"+<linkpartner> - ALE link established to this partner
// "AXNMGS"+<linkpartner>+<two digit message length in decimal>+<message text>
//    OK or EV response after sending
//    "AIAMDM"+<message> - ALE message received

};

struct barrett_radio_state barrett[MAX_CLIENTS];

unsigned char barrett_e0_string[6]={0x13,'E','0',13,10,0x11};
unsigned char barrett_ok_string[6]={0x13,'O','K',13,10,0x11};

int hfbarrett_read_byte(int i,unsigned char c)
{
  if (c==0x15) {
    // Control-U -- clear input buffer
    clients[i].buffer_count=0;
  } else if ((c!='\n')&&(c!='\r')&&c) {
    // fprintf(stderr,"Radio #%d received character 0x%02x\n",i,c);

    // No echo by default for Barrett radios
    // write(clients[i].socket,&c,1);
    
    if (clients[i].buffer_count<(CLIENT_BUFFER_SIZE-1))
      clients[i].buffer[clients[i].buffer_count++]=c;
  } else {
    if (clients[i].buffer_count) {

      // No CRLF echo by default for Barrett radios
      // write(clients[i].socket,"\r\n",2);
      
      clients[i].buffer[clients[i].buffer_count]=0;
      fprintf(stderr,"Barrett HF Radio #%d sent command '%s'\n",i,clients[i].buffer);

      // Process the command here
      if (!strncasecmp("AXNMSG",(char *)clients[i].buffer,6)) {
	// Send ALE message
	// XXX- implement me!
	fprintf(stderr,"Saw AXNMSG command\n");
      } else if (!strncasecmp("ARAMDM",(char *)clients[i].buffer,6)) {
	// [un]Register for AMD messages
	switch (clients[i].buffer[6]) {
	case '1': case '0':
	  barrett[i].aramdm=clients[i].buffer[6]-'0';
	  write(clients[i].socket,barrett_ok_string,6);
	  break;
	default:
	  write(clients[i].socket,barrett_e0_string,6);
	}
      } else if (!strncasecmp("ARAMDP",(char *)clients[i].buffer,6)) {
	// [un]Register for phone messages
	switch (clients[i].buffer[6]) {
	case '0': case '1':
	  barrett[i].aramdp=clients[i].buffer[6]-'0';
	  write(clients[i].socket,barrett_ok_string,6);
	  break;
	default:
	  write(clients[i].socket,barrett_e0_string,6);
	}
      } else if (!strncasecmp("ARCALL",(char *)clients[i].buffer,6)) {
	// [un]Register for new calls
	switch (clients[i].buffer[6]) {
	case '0': case '1':
	  barrett[i].arcall=clients[i].buffer[6]-'0';
	  write(clients[i].socket,barrett_ok_string,6);
	  break;
	default:
	  write(clients[i].socket,barrett_e0_string,6);
	}
      } else if (!strncasecmp("ARLINK",(char *)clients[i].buffer,6)) {
	// [un]Register for new calls
	switch (clients[i].buffer[6]) {
	case '0': case '1':
	  barrett[i].arlink=clients[i].buffer[6]-'0';
	  write(clients[i].socket,barrett_ok_string,6);
	  break;
	default:
	  write(clients[i].socket,barrett_e0_string,6);
	}
      } else if (!strncasecmp("ARLTBL",(char *)clients[i].buffer,6)) {
	// [un]Register for new calls
	switch (clients[i].buffer[6]) {
	case '0': case '1':
	  barrett[i].arltbl=clients[i].buffer[6]-'0';
	  write(clients[i].socket,barrett_ok_string,6);
	  break;
	default:
	  write(clients[i].socket,barrett_e0_string,6);
	}
      } else if (!strncasecmp("ARMESS",(char *)clients[i].buffer,6)) {
	// [un]Register for new calls
	switch (clients[i].buffer[6]) {
	case '0': case '1':
	  barrett[i].armess=clients[i].buffer[6]-'0';
	  write(clients[i].socket,barrett_ok_string,6);
	  break;
	default:
	  write(clients[i].socket,barrett_e0_string,6);
	}
      } else if (!strncasecmp("ARSTAT",(char *)clients[i].buffer,6)) {
	// [un]Register for new calls
	switch (clients[i].buffer[6]) {
	case '0': case '1':
	  barrett[i].arstat=clients[i].buffer[6]-'0';
	  write(clients[i].socket,barrett_ok_string,6);
	  break;
	default:
	  write(clients[i].socket,barrett_e0_string,6);
	}
      } else {
	// Complain about unknown commands
	fprintf(stderr,"Responding with Barrett E0 string\n");
	write(clients[i].socket,barrett_e0_string,6);
      }

      // Reset buffer ready for next command
      clients[i].buffer_count=0;
    }    
  }

  return 0;
}

int hfbarrett_heartbeat(int client)
{
  return 0;
}

int hfbarrett_encapsulate_packet(int from,int to,
				 unsigned char *packet,
				 int *packet_len)
{
  return 0;
}
