/*
  Import Rhizome bundles into a block store.

*/

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sync.h"
#include "lbard.h"
#include "blockstore.h"

// Satisfy dependencies of code pulled in from LBARD
#include "radio_type.h"
int debug_insert=1;
char *my_sid_hex="NOT SET";
radio_type radio_types[]={
			  {-1,NULL,NULL,NULL,NULL,NULL,NULL,NULL,-1}
};
long long last_servald_contact=0;


void usage(void)
{
  printf("usage: rhizomeimport <servald server> <credential> <blockstore descriptor>\n"
	 "\n"
	 "   servald server        - IP:port of Servald server\n"
	 "   credential            - user:pass for servald Rhizome RESTful API\n"
	 "   blockstore descriptor - Argument passed when opening/creating block store.\n"
	 );
  exit(-1);
}


int main(int argc,char **argv)
{
  if (argc!=4) {
    usage();
  }

  char *servald_server=argv[1];
  char *credential=argv[2];
  char *blockstore_descriptor=argv[3];
  char token[1024]="";

  // Re-use normal LBARD rhizome async fetch.
  // It doesn't tell us explicitly when done, so we have to infer a bit
  errno=EAGAIN;
  while (errno==EAGAIN) {
    if (load_rhizome_db_async(servald_server,credential,token)) {
      break;
    }
  }
}

int register_bundle(char *service,
                    char *bid,
                    char *version,
                    char *author,
                    char *originated_here,
                    long long length,
                    char *filehash,
                    char *sender,
                    char *recipient,
                    char *name)
{
  return 0;
}
