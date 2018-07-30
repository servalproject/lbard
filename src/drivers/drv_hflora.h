int hflora_serviceloop(int serialfd);
int hflora_process_line(char *l);
int hflora_receive_bytes(unsigned char *bytes,int count);
int hflora_send_packet(int serialfd,unsigned char *out, int len);
int hflora_radio_detect(int fd);
int hflora_check_if_ready(void);
//int hflora_initialise(int serialfd);