#include <dev/blk_dev.h>
#include <printk.h>

dev::blk_dev::~blk_dev(void) {}

bool dev::blk_dev::read(u64 offset, u32 len, void *data) {
  u64 bsize = block_size();
  assert((offset % bsize) == 0);
  assert((len % bsize) == 0);

  u64 first_block = offset / bsize;
  u64 end_block = (offset + len) / bsize;

  u64 blk_count = end_block - first_block;

  for (u64 i = 0; i < blk_count; i++)
    if (!read_block(first_block + i, (u8 *)data + bsize * i)) return false;

  return true;
}

bool dev::blk_dev::write(u64 offset, u32 len, const void *data) {
  u64 bsize = block_size();
  assert((offset % bsize) == 0);
  assert((len % bsize) == 0);

  u64 first_block = offset / bsize;
  u64 end_block = (offset + len) / bsize;

  u64 blk_count = end_block - first_block;

  for (u64 i = 0; i < blk_count; i++)
    if (!write_block(first_block + i, (u8 *)data + bsize * i)) return false;

  return true;
}
