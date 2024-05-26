#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wafel/dynamic.h>
#include <wafel/ios_dynamic.h>
#include <wafel/utils.h>
#include <wafel/patch.h>
#include <wafel/ios/svc.h>
#include <wafel/trampoline.h>
#include "wafel/ios/prsh.h"
#include "wafel/hai.h"
#include "mbr.h"
#include "sal.h"

const char* MODULE_NAME = "USBPARTITION";

#define SECTOR_SIZE 512
#define LOCAL_HEAP_ID 0xCAFE
#define DEVTYPE_USB 17
#define DEVTYPE_SD 6

// tells crypto to not do crypto (depends on stroopwafel patch)
#define NO_CRYPTO_HANDLE 0xDEADBEEF

#define LD_DWORD(ptr)       (u32)(((u32)*((u8*)(ptr)+3)<<24)|((u32)*((u8*)(ptr)+2)<<16)|((u16)*((u8*)(ptr)+1)<<8)|*(u8*)(ptr))

#define FIRST_HANDLE ((int*)0x11c39e78)
#define HANDLE_END ((int*)0x11c3a420)

#ifdef MOUNT_SD
FSSALAttachDeviceArg extra_attach_arg;
#endif

static u32 sdusb_offset = 0xFFFFFFF;
static u32 sdusb_size = 0xFFFFFFFF;
static char umsBlkDevID[0x10] ALIGNED(2);

#ifdef USE_MLC_KEY
u32 mlc_size_sectors = 0;
#endif

static volatile bool learn_mlc_crypto_handle = false;
static volatile bool learn_usb_crypto_handle = false;

static read_func *real_read;
static write_func *real_write;
static sync_func *real_sync;

bool active = false;

#define ADD_OFFSET(high, low) do { \
    unsigned long long combined = ((unsigned long long)(high) << 32) | (low); \
    combined += sdusb_offset; \
    (high) = (unsigned int)(combined >> 32); \
    (low) = (unsigned int)(combined & 0xFFFFFFFF); \
} while (0)

int read_wrapper(void *device_handle, u32 lba_hi, u32 lba_lo, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    ADD_OFFSET(lba_hi, lba_lo);
    return real_read(device_handle, lba_hi, lba_lo, blkCount, blockSize, buf, cb, cb_ctx);
}

int write_wrapper(void *device_handle, u32 lba_hi, u32 lba_lo, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    ADD_OFFSET(lba_hi, lba_lo);
    return real_write(device_handle, lba_hi, lba_lo, blkCount, blockSize, buf, cb, cb_ctx);
}

int sync_wrapper(int server_handle, u32 lba_hi, u32 lba_lo, u32 num_blocks, void * cb, void * cb_ctx){
    ADD_OFFSET(lba_hi, lba_lo);
    //debug_printf("%s: sync called lba: %d, num_blocks: %d\n", MODULE_NAME, lba_lo, num_blocks);
    return real_sync(server_handle, lba_hi, lba_lo, num_blocks, cb, cb_ctx);
}

static void hai_write_file_patch(trampoline_t_state *s){
    uint32_t *buffer = (uint32_t*)s->r[1];
    debug_printf("HAI WRITE COMPANION\n");
    if(active && hai_getdev() == DEVTYPE_USB){
        hai_companion_add_offset(buffer, sdusb_offset);
    }
}

static int hai_umsBlkDevId_patch(int entry_id, char* umsdev_id, size_t size, int r3, int(*hai_param_add)(int, char*, size_t)){
    if(active && hai_getdev() == DEVTYPE_USB){
        debug_printf("%s: Patching umsdev id to %016llX..\n", MODULE_NAME, *(u64*)umsdev_id);
        umsdev_id = umsBlkDevID;
        size = sizeof(umsBlkDevID);
    }
    return hai_param_add(entry_id, umsdev_id, size);
}

static void apply_hai_patches(void){
    trampoline_t_hook_before(0x050078AE, hai_write_file_patch);
    // hai_write_file_patch needs to know the hai dev
    hai_apply_getdev_patch();

    // replace devid with bytes from MBR (where HAI will look)
    trampoline_t_blreplace(0x0500900a, hai_umsBlkDevId_patch);
}

static bool is_mbr(mbr_sector* mbr){
    debug_printf("%s: MBR signature: 0x%04X\n", MODULE_NAME, mbr->boot_signature);
    return mbr->boot_signature==0x55AA;
}

static partition_entry* find_usb_partition(mbr_sector* mbr){
    partition_entry *selected = NULL;
    u32 selected_start = 0;
    for (size_t i = 1; i < MBR_MAX_PARTITIONS; i++){
        u32 istart = LD_DWORD(mbr->partition[i].lba_start);
        if(mbr->partition[i].type == NTFS && (selected_start < istart)){
            selected = mbr->partition+i;
            selected_start = istart;
        }
    }
    return selected;
}

struct cb_ctx {
    int semaphore;
    int res;
} typedef cb_ctx;

static void read_callback(int res, cb_ctx *ctx){
    ctx->res = res;
    iosSignalSemaphore(ctx->semaphore);
}


static int sync_read(FSSALAttachDeviceArg* attach_arg, u64 lba, u32 blkCount, void *buf){
    cb_ctx ctx = {iosCreateSemaphore(1,0)};
    if(ctx.semaphore < 0){
        debug_printf("%s: Error creating Semaphore: 0x%X\n", MODULE_NAME, ctx.semaphore);
        return ctx.semaphore;
    }
    int res = attach_arg->op_read(attach_arg->server_handle, lba>>32, (u32)lba, blkCount, SECTOR_SIZE, buf, read_callback, &ctx);
    if(!res){
        iosWaitSemaphore(ctx.semaphore, 0);
        res = ctx.res;
    }
    iosDestroySemaphore(ctx.semaphore);
    return res;
}

void patch_usb_attach_handle(FSSALAttachDeviceArg *attach_arg){
    real_read = attach_arg->op_read;
    real_write = attach_arg->op_write;
    real_sync = attach_arg->opsync;
    attach_arg->op_read = read_wrapper;
    attach_arg->op_write = write_wrapper;
    attach_arg->op_read2 = crash_and_burn;
    attach_arg->op_write2 = crash_and_burn;
    attach_arg->opsync = sync_wrapper;
    attach_arg->params.device_type = DEVTYPE_USB;
    attach_arg->params.max_lba_size = sdusb_size -1;
    attach_arg->params.block_count = sdusb_size;
}

#ifdef MOUNT_SD
int clone_patch_attach_sd_hanlde(FSSALAttachDeviceArg *attach_arg){
    memcpy(&extra_attach_arg, attach_arg, sizeof(FSSALAttachDeviceArg));
    extra_attach_arg.params.device_type = DEVTYPE_SD;
    //attach_arg->params.device_type = DEVTYPE_SD;
    debug_printf("%s: Attaching USB storage as SD\n", MODULE_NAME);
    int res = FSSAL_attach_device(&extra_attach_arg);
    //int res = FSSAL_attach_device(attach_arg);
    debug_printf("%s: Attached extra handle. res: 0x%X\n", MODULE_NAME, res);
    return res;
}
#endif

int read_usb_partition_from_mbr(FSSALAttachDeviceArg *attach_arg, u32* out_offset, u32* out_size){
    mbr_sector *mbr = iosAllocAligned(LOCAL_HEAP_ID, SECTOR_SIZE, 0x40);
    if(!mbr){
        debug_printf("%s: Failed to allocate IO buf\n", MODULE_NAME);
        return -1;
    }
    int ret = -2;
    int res = sync_read(attach_arg, 0, 1, mbr);
    if(res)
        goto out_free;

    if(!is_mbr(mbr)){
        debug_printf("%s: MBR NOT found!!!\n", MODULE_NAME);
        ret = 0;
        goto out_free;
    }

    partition_entry *part = find_usb_partition(mbr);
    if(!part){
        debug_printf("%s: USB partition not found!!!\n", MODULE_NAME);
        ret = 1;
        goto out_free;
    }
    ret = 2;
    *out_offset = LD_DWORD(part->lba_start);
    *out_size = LD_DWORD(part->lba_length);
    memcpy(umsBlkDevID, mbr, sizeof(umsBlkDevID));
    debug_printf("%s: USB partition found %p: offset: %u, size: %u\n", MODULE_NAME, part, *out_offset, *out_size);

out_free:
    iosFree(LOCAL_HEAP_ID, mbr); // also frees part
    return ret;
}

int usb_attach_hook(FSSALAttachDeviceArg *attach_arg, int r1, int r2, int r3, int (*sal_attach)(FSSALAttachDeviceArg*)){
    int res = read_usb_partition_from_mbr(attach_arg, &sdusb_offset, &sdusb_size);

    int ret = 0;

#ifdef MOUNT_SD
    if(res>0) // MBR detected
        ret = clone_patch_attach_sd_hanlde(attach_arg);
#endif

    if(res==2) {
        patch_usb_attach_handle(attach_arg);
        active = true;
    }
    
    if(res == 0 || res == 2){ // direct or partitioned
        debug_printf("Attatching USB partition\n");
        ret = sal_attach(attach_arg);
        learn_usb_crypto_handle = true;
    }

    return ret;
}

#ifdef USE_MLC_KEY
int mlc_attach_hook(int* attach_arg, int r1, int r2, int r3, int (*attach_fun)(int*)){
    mlc_size_sectors = attach_arg[0xe - 3];
    learn_mlc_crypto_handle = true;
    return attach_fun(attach_arg);
}
#endif

static void crypto_hook(trampoline_state *state){
#ifdef USE_MLC_KEY
    static u32 mlc_crypto_handle = 0;
    if(learn_mlc_crypto_handle && state->r[5] == mlc_size_sectors){
        learn_mlc_crypto_handle = false;
        mlc_crypto_handle = state->r[0];
        debug_printf("%s: learned mlc crypto handle: 0x%X\n", MODULE_NAME, mlc_crypto_handle);
    }
#endif

    static u32 usb_crypto_handle = 0;
    if(state->r[5] == sdusb_size){
        if(learn_usb_crypto_handle){
            learn_usb_crypto_handle = false;
            usb_crypto_handle = state->r[0];
            debug_printf("%s: learned mlc crypto handle: 0x%X\n", MODULE_NAME,  usb_crypto_handle);
        }
        if(usb_crypto_handle == state->r[0]){
#ifdef USE_MLC_KEY
            state->r[0] = mlc_crypto_handle;
#else     
            state->r[0] = NO_CRYPTO_HANDLE;
#endif
        }
    }
}

void test_hook(trampoline_state* state){
    int *data = (int*)state->r[2];
    for(int i=0; i<16; i++){
        debug_printf("%X: %08X\n", i, data[i]);
    }
}

// This fn runs before everything else in kernel mode.
// It should be used to do extremely early patches
// (ie to BSP and kernel, which launches before MCP)
// It jumps to the real IOS kernel entry on exit.
__attribute__((target("arm")))
void kern_main()
{
    // Make sure relocs worked fine and mappings are good
    debug_printf("we in here %s plugin kern %p\n", MODULE_NAME, kern_main);

    debug_printf("init_linking symbol at: %08x\n", wafel_find_symbol("init_linking"));

    trampoline_blreplace(0x1077eea8, usb_attach_hook);
    //trampoline_hook_before(0x10740f48, crypto_hook); // hook decrypt call
    //trampoline_hook_before(0x10740fe8, crypto_hook); // hook encrypt call

#ifdef USE_MLC_KEY
    trampoline_blreplace(0x107bdae0, mlc_attach_hook);
#endif

    // somehow it causes crashes when applied from the attach hook
    apply_hai_patches();

    debug_printf("%s: patches applied\n", MODULE_NAME);

    //trampoline_hook_before(0x10740f2c, test_hook);
}

// This fn runs before MCP's main thread, and can be used
// to perform late patches and spawn threads under MCP.
// It must return.
void mcp_main()
{

}
