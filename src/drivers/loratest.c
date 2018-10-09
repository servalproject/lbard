#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>

#include "drv_lora.h"
/*#include "sync.h"
#include "lbard.h"
#include "serial.h"
#include "version.h"
#include "radios.h"
#include "hf.h"
#include "code_instrumentation.h" */

int fd = -1;

int main(int argc, char **argv)
{
    fd = open("/dev/ttyUSB0",O_RDWR);
    printf("main result : %d\n",rfdlora_radio_detect(fd));

    return 0;
}