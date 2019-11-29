int hfcodan3012_radio_detect(int fd);
int hfcodan3012_serviceloop(int serialfd);
int hfcodan3012_process_line(char *l);
int hfcodan3012_receive_bytes(unsigned char *bytes,int count);
int hfcodan3012_send_packet(int serialfd,unsigned char *out, int len);
