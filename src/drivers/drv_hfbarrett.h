int hfbarrett_serviceloop(int serialfd);
int hfbarrett_process_line(char *l);
int hfbarrett_receive_bytes(unsigned char *bytes,int count);
int hfbarrett_send_packet(int serialfd,unsigned char *out, int len);
