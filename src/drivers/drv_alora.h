int rfdlora_serviceloop(int serialfd);
int rfdlora_process_line(char *l);
int rfdlora_receive_bytes(unsigned char *bytes,int count);
int rfdlora_send_packet(int serialfd,unsigned char *out, int len);
int rfdlora_radio_detect(int fd);
int rfdlora_check_if_ready(void);
//int lora_initialise(int serialfd);
