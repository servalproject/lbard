void hex2str(unsigned char *input, unsigned char *output, int input_len);
void str2hex(unsigned char *input, char *output, int input_len);
int lorarn_radio_detect(int fd);
int lorarn_serviceloop(int serialfd);
int lorarn_receive_bytes(unsigned char *bytes,int count);
int lorarn_send_packet(int serialfd,unsigned char *out, int len);
int lorarn_check_if_ready(void);

  
