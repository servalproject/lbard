int hf2020_serviceloop(int serialfd);
int hf2020_process_line(char *l);
int hf2020_receive_bytes(unsigned char *bytes,int count);
int hf2020_send_packet(int serialfd,unsigned char *out, int len);
int hf2020_ready_test(void);
int hf2020_my_turn_to_send(void);
int hf2020_radio_detect(int serialfd);
