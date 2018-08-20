int alora_serviceloop(int serialfd);
int alora_process_line(char *l);
int alora_receive_bytes(unsigned char *bytes,int count);
int alora_send_packet(int serialfd,unsigned char *out, int len);
int alora_radio_detect(int fd);
int alora_check_if_ready(void);
//int lora_initialise(int serialfd);