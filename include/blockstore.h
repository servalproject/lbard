void *blockstore_create(unsigned long long initial_size, unsigned long long maximum_size,char *device);
int blockstore_retrieve(void *bs_in,unsigned char *hash,int hash_len,unsigned char *block, int* block_len);
int blockstore_store(void *bs_in,unsigned char *hash,int hash_len,unsigned char *block, int block_len);

#define BS_HASH_SIZE 24
#define BS_MAX_HASH_SIZE 32
#define BS_MAX_BLOCKSIZE 200

struct block {
  unsigned char hash[BS_MAX_HASH_SIZE];
  unsigned char data[BS_MAX_BLOCKSIZE];
  unsigned char data_len;
  unsigned char hash_len;
};




