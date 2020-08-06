#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv)
{
  if (argc!=3) {
    fprintf(stderr,"ERROR: usage: mktestfile <instance> <size number>\n");
    exit(-1);
  }
  int n=atoi(argv[2]);
  char msg[1024];

  snprintf(msg,1024,"Test file %s/%s                                                                                    ",
	   argv[1],argv[2]);
  msg[63]='\n';
  msg[64]=0;
  printf("%s",msg);
  
  for(int i=0;i<n;n++) {
    snprintf(msg,1024,"Data block %d of file %s/%s                                                                      ",
	     i,argv[1],argv[2]);
    msg[63]='\n';
    msg[64]=0;
    printf("%s",msg);
  }
}
