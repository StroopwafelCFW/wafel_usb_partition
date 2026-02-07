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
#include "ums_lba64.h"
#include "wfs.h"

static bool active = false;
static char umsBlkDevID[0x10] ALIGNED(4);

#ifdef MOUNT_SD
static FSSALAttachDeviceArg extra_attach_arg;
static int umsDeviceCount = 0;
static int noMBRCount = 0;

/**
 * @brief Allows other plugins to check if they should wait for a emulated SD
 * 
 * @return Returns true if at least one UMS device has a MBR or not all UMS devices have been initialized yet
 */

bool wafel_usb_partition_wait_usbsd(void){
    return umsDeviceCount > noMBRCount;
}

static bool hook_ums_device_initilize(trampoline_state* state){
    debug_printf("%s ums_device_initialize called device=%p\n", PLUGIN_NAME, (void*)state->r[0]);
    umsDeviceCount++;
    return false;
}
#endif

static volatile void* usb_server_handle = 0;
static volatile bool usb_handle_set = false;

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
#define MAX_CLONED_HANDLES 4
static FSSALHandle *cloned_handles[MAX_CLONED_HANDLES][2] = {NULL};

static FSSALHandle* clone_patch_attach_sd_hanlde(FSSALAttachDeviceArg *attach_arg){
    memcpy(&extra_attach_arg, attach_arg, sizeof(FSSALAttachDeviceArg));
    extra_attach_arg.params.device_type = DEVTYPE_SD;
    if(extra_attach_arg.params.block_count > UINT32_MAX){
        extra_attach_arg.params.block_count = UINT32_MAX;
        extra_attach_arg.params.max_lba_size = UINT32_MAX -1;
    }
    debug_printf("%s: Attaching USB storage as SD\n", PLUGIN_NAME);
    FSSALHandle *res = FSSAL_attach_device(&extra_attach_arg);
    debug_printf("%s: Attached extra handle. res: 0x%X\n", PLUGIN_NAME, res);
    return res;
}
#endif

static int dummy(){
    return -1;
}

static void patch_dummy_attach_arg(FSSALAttachDeviceArg *attach_arg){
    attach_arg->op_read = dummy;
    attach_arg->op_write = dummy;
    attach_arg->op_read2 = dummy;
    attach_arg->op_write2 = dummy;
    attach_arg->opsync = dummy;
    attach_arg->params.max_lba_size = 0;
    attach_arg->params.block_count = 0;
}


static FSSALHandle *partition_handle = NULL;
FSSALHandle* usb_attach_hook(FSSALAttachDeviceArg *attach_arg, int r1, int r2, int r3, FSSALHandle* (*sal_attach)(FSSALAttachDeviceArg*)){
    if(attach_arg->params.device_type == DEVTYPE_SD){
        debug_printf("%s: Already SD device type, skipping attach hook\n", PLUGIN_NAME);
        return sal_attach(attach_arg);
    }
    
    u32 part_offset, part_size;
    int res = read_usb_partition_from_mbr(attach_arg, &part_offset, &part_size, umsBlkDevID);

    FSSALHandle *sd_handle = NULL;
#ifdef MOUNT_SD
    if(res>0) { // MBR detected
        debug_printf("%s: MBR detected, attaching for SD\n", PLUGIN_NAME);
        sd_handle = clone_patch_attach_sd_hanlde(attach_arg);
        debug_printf("%s: Attached for SD, res: 0x%X\n", PLUGIN_NAME, sd_handle);
    } else
        noMBRCount++;
#endif

    if (res==1) {
        debug_printf("%s: No WFS detected, creating dummy USB device\n", PLUGIN_NAME);
        patch_dummy_attach_arg(attach_arg);
    } else if(res==2 && !active) {
        active = true;
        partition_offset = part_offset;
        partition_size = part_size;
        patch_partition_attach_arg(attach_arg);
        usb_server_handle = attach_arg->server_handle;
        usb_handle_set = true;
    } 
    
    debug_printf("%s: Attatching USB\n", PLUGIN_NAME);
    FSSALHandle *usb_handle = sal_attach(attach_arg);
    debug_printf("%s: Attached USB\n", PLUGIN_NAME);
    if(res==2 && !partition_handle) {
        partition_handle = usb_handle;
    }

    if(!sd_handle)
        return usb_handle;
    if(!usb_handle)
        return sd_handle;

#ifdef MOUNT_SD
    for(int i=0; i<MAX_CLONED_HANDLES; i++){
        if(cloned_handles[i][0] == 0){
            cloned_handles[i][0] = usb_handle;
            cloned_handles[i][1] = sd_handle;
            debug_printf("%s: Cloned handle pair: USB: %d SD: %d\n", PLUGIN_NAME, usb_handle, sd_handle);
            return usb_handle;
        }
    }
    debug_printf("%s: No space for cloned handle, returning USB handle\n", PLUGIN_NAME);
#endif
    return usb_handle;
}

void usb_detach_hook(FSSALHandle *device_handle, int r1, int r2, int r3, void (*sal_detach)(FSSALHandle*)){
#ifdef MOUNT_SD
    for(int i=0; i<MAX_CLONED_HANDLES; i++){
        if(cloned_handles[i][0] == device_handle){
            debug_printf("%s: Detaching cloned handle pair: USB: %d SD: %d\n", PLUGIN_NAME, cloned_handles[i][0], cloned_handles[i][1]);
            sal_detach(cloned_handles[i][1]);
            cloned_handles[i][0] = cloned_handles[i][1] = NULL;
        }
    }
#endif
    sal_detach(device_handle);
    if(device_handle == partition_handle){
        debug_printf("%s: Detached partition handle, deactivating partition patching\n", PLUGIN_NAME);
        active = false;
        partition_handle = NULL;
    }
}


static void wfs_initDeviceParams_exit_hook(trampoline_state *regs){
    WFS_Device *wfs_device = (WFS_Device*)regs->r[5];
    FSSALDevice *sal_device = FSSAL_LookupDevice(wfs_device->handle);
    void *server_handle = sal_device->server_handle;
    debug_printf("wfs_initDeviceParams_exit_hook server_handle: %p\n", server_handle);
    if(usb_handle_set && server_handle == usb_server_handle) {
#ifdef USE_MLC_KEY
        wfs_device->crypto_key_handle = WFS_KEY_HANDLE_MLC;
#elif defined(NOCRYPTO)
        wfs_device->crypto_key_handle = WFS_KEY_HANDLE_NOCRYPTO;
#endif
    }
}

static void test_hook(trampoline_state* state){
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

    patch_ums_lba64();

    trampoline_blreplace(0x1077eea8, usb_attach_hook);
    trampoline_blreplace(0x1077eed4, usb_detach_hook);

#if defined(USE_MLC_KEY) || defined(NOCRYPTO)
    trampoline_hook_before(0x107435f4, wfs_initDeviceParams_exit_hook);
#endif

    // somehow it causes crashes when applied from the attach hook
    apply_hai_patches();

#ifdef MOUNT_SD
    // prfile look at the first partition only
    ASM_PATCH_K(0x10793234, "cmp r4, r4");
    trampoline_hook_before_v2(0x10782034, hook_ums_device_initilize);
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
