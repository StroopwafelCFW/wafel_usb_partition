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
#include "sal.h"
#include "sal_partition.h"
#include "sal_mbr.h"

// tells crypto to not do crypto (depends on stroopwafel patch)
#define NO_CRYPTO_HANDLE 0xDEADBEEF

static bool active = false;
static char umsBlkDevID[0x10] ALIGNED(4);

#ifdef MOUNT_SD
FSSALAttachDeviceArg extra_attach_arg;
#endif

#ifdef USE_MLC_KEY
u32 mlc_size_sectors = 0;
#endif

static volatile bool learn_mlc_crypto_handle = false;
static volatile bool learn_usb_crypto_handle = false;

static void hai_write_file_patch(trampoline_t_state *s){
    uint32_t *buffer = (uint32_t*)s->r[1];
    debug_printf("HAI WRITE COMPANION\n");
    if(active && hai_getdev() == DEVTYPE_USB){
        hai_companion_add_offset(buffer, partition_offset);
    }
}

static int hai_umsBlkDevId_patch(int entry_id, char* umsdev_id, size_t size, int r3, int(*hai_param_add)(int, char*, size_t)){
    if(active && hai_getdev() == DEVTYPE_USB){
        debug_printf("%s: Patching umsdev id to %016llX..\n", PLUGIN_NAME, *(u64*)umsdev_id);
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

#ifdef MOUNT_SD
int clone_patch_attach_sd_hanlde(FSSALAttachDeviceArg *attach_arg){
    memcpy(&extra_attach_arg, attach_arg, sizeof(FSSALAttachDeviceArg));
    extra_attach_arg.params.device_type = DEVTYPE_SD;
    if(extra_attach_arg.params.block_count > UINT32_MAX){
        extra_attach_arg.params.block_count = UINT32_MAX;
        extra_attach_arg.params.max_lba_size = UINT32_MAX -1;
    }
    debug_printf("%s: Attaching USB storage as SD\n", PLUGIN_NAME);
    int res = FSSAL_attach_device(&extra_attach_arg);
    debug_printf("%s: Attached extra handle. res: 0x%X\n", PLUGIN_NAME, res);
    return res;
}
#endif

int usb_attach_hook(FSSALAttachDeviceArg *attach_arg, int r1, int r2, int r3, int (*sal_attach)(FSSALAttachDeviceArg*)){
    int res = read_usb_partition_from_mbr(attach_arg, &partition_offset, &partition_size, umsBlkDevID);

    int ret = 0;

#ifdef MOUNT_SD
    if(res>0) // MBR detected
        ret = clone_patch_attach_sd_hanlde(attach_arg);
#endif

    if(res==2) {
        patch_partition_attach_arg(attach_arg, DEVTYPE_USB);
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
        debug_printf("%s: learned mlc crypto handle: 0x%X\n", PLUGIN_NAME, mlc_crypto_handle);
    }
#endif

    static u32 usb_crypto_handle = 0;
    if(state->r[5] == partition_size){
        if(learn_usb_crypto_handle){
            learn_usb_crypto_handle = false;
            usb_crypto_handle = state->r[0];
            debug_printf("%s: learned mlc crypto handle: 0x%X\n", PLUGIN_NAME,  usb_crypto_handle);
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
    debug_printf("we in here %s plugin kern %p\n", PLUGIN_NAME, kern_main);

    debug_printf("init_linking symbol at: %08x\n", wafel_find_symbol("init_linking"));

    trampoline_blreplace(0x1077eea8, usb_attach_hook);
    //trampoline_hook_before(0x10740f48, crypto_hook); // hook decrypt call
    //trampoline_hook_before(0x10740fe8, crypto_hook); // hook encrypt call

#ifdef USE_MLC_KEY
    trampoline_blreplace(0x107bdae0, mlc_attach_hook);
#endif

    // somehow it causes crashes when applied from the attach hook
    apply_hai_patches();

#ifdef MOUNT_SD
    // prfile look at the first partition
    ASM_PATCH_K(0x10793234, "cmp r4, r4");
#endif

    debug_printf("%s: patches applied\n", PLUGIN_NAME);

    //trampoline_hook_before(0x10740f2c, test_hook);
}

// This fn runs before MCP's main thread, and can be used
// to perform late patches and spawn threads under MCP.
// It must return.
void mcp_main()
{

}
