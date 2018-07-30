int hflora_serviceloop(int serialfd);
int hflora_process_line(char *l);
int hflora_receive_bytes(unsigned char *bytes,int count);
int hflora_send_packet(int serialfd,unsigned char *out, int len);
