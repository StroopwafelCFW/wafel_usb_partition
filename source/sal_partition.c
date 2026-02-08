#include <string.h>
#include <wafel/ios/svc.h>
#include <wafel/utils.h>
#include "sal.h"
#include "sal_partition.h"

#define SECTOR_SIZE 512

typedef struct {
    read_func *real_read;
    write_func *real_write;
    sync_func *real_sync;
    u32 offset;
} PartitionContext;

static PartitionContext contexts[2] = {
    {.offset = 0xFFFFFFF},
    {.offset = 0xFFFFFFF}
};

u32 get_partition_offset(void){
    return contexts[0].offset;
}

#define ADD_OFFSET_VAL(offset_val, high, low) do { \
    unsigned long long combined = ((unsigned long long)(high) << 32) | (low); \
    combined += offset_val; \
    (high) = (unsigned int)(combined >> 32); \
    (low) = (unsigned int)(combined & 0xFFFFFFFF); \
} while (0)

static int read_wrapper0(void *device_handle, u32 lba_hi, u32 lba_lo, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    ADD_OFFSET_VAL(contexts[0].offset, lba_hi, lba_lo);
    return contexts[0].real_read(device_handle, lba_hi, lba_lo, blkCount, blockSize, buf, cb, cb_ctx);
}
static int write_wrapper0(void *device_handle, u32 lba_hi, u32 lba_lo, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    ADD_OFFSET_VAL(contexts[0].offset, lba_hi, lba_lo);
    return contexts[0].real_write(device_handle, lba_hi, lba_lo, blkCount, blockSize, buf, cb, cb_ctx);
}
static int sync_wrapper0(int server_handle, u32 lba_hi, u32 lba_lo, u32 num_blocks, void * cb, void * cb_ctx){
    ADD_OFFSET_VAL(contexts[0].offset, lba_hi, lba_lo);
    return contexts[0].real_sync(server_handle, lba_hi, lba_lo, num_blocks, cb, cb_ctx);
}

static int read_wrapper1(void *device_handle, u32 lba_hi, u32 lba_lo, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    ADD_OFFSET_VAL(contexts[1].offset, lba_hi, lba_lo);
    return contexts[1].real_read(device_handle, lba_hi, lba_lo, blkCount, blockSize, buf, cb, cb_ctx);
}
static int write_wrapper1(void *device_handle, u32 lba_hi, u32 lba_lo, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    ADD_OFFSET_VAL(contexts[1].offset, lba_hi, lba_lo);
    return contexts[1].real_write(device_handle, lba_hi, lba_lo, blkCount, blockSize, buf, cb, cb_ctx);
}
static int sync_wrapper1(int server_handle, u32 lba_hi, u32 lba_lo, u32 num_blocks, void * cb, void * cb_ctx){
    ADD_OFFSET_VAL(contexts[1].offset, lba_hi, lba_lo);
    return contexts[1].real_sync(server_handle, lba_hi, lba_lo, num_blocks, cb, cb_ctx);
}


static void readop2_crash(int *device_handle, u32 lba_hi, u32 lba, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    debug_printf("%s ERROR: readop2 was called!!!! handle: %p type: %u\n", PLUGIN_NAME, device_handle, device_handle[5]);
    crash_and_burn();
}

static void writeop2_crash(int *device_handle, u32 lba_hi, u32 lba, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    debug_printf("%s ERROR: readop2 was called!!!! handle: %p type: %u\n", PLUGIN_NAME, device_handle, device_handle[5]);
    crash_and_burn();
}

void patch_partition_attach_arg(FSSALAttachDeviceArg *attach_arg, int index, u32 offset, u32 size){
    if (index < 0 || index > 1) return;

    contexts[index].real_read = attach_arg->op_read;
    contexts[index].real_write = attach_arg->op_write;
    contexts[index].real_sync = attach_arg->opsync;
    contexts[index].offset = offset;

    if (index == 0) {
        attach_arg->op_read = read_wrapper0;
        attach_arg->op_write = write_wrapper0;
        attach_arg->opsync = sync_wrapper0;
    } else {
        attach_arg->op_read = read_wrapper1;
        attach_arg->op_write = write_wrapper1;
        attach_arg->opsync = sync_wrapper1;
    }

    attach_arg->op_read2 = readop2_crash;
    attach_arg->op_write2 = writeop2_crash;
    //attach_arg->params.device_type = device_type;
    attach_arg->params.max_lba_size = size -1;
    attach_arg->params.block_count = size;
}