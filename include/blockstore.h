void *blockstore_create(unsigned long long initial_size, unsigned long long maximum_size,char *device);
int blockstore_retrieve(void *bs_in,unsigned char *hash,int hash_len,unsigned char *block, int* block_len);
int blockstore_store(void *bs_in,unsigned char *hash,int hash_len,unsigned char *block, int block_len);



