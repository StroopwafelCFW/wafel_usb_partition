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

typedef struct {
    char umsBlkDevID[0x10] ALIGNED(4);
    bool active;
} HAIContext;

static HAIContext hai_ctx ALIGNED(4) = {{0}, false};

#ifdef MOUNT_SD
static FSSALAttachDeviceArg extra_attach_arg;

/**
 * @brief Allows other plugins to check if they should wait for a emulated SD
 * 
 * @return Returns true if at least one UMS device has a MBR or not all UMS devices have been initialized yet
 */

bool wafel_usb_partition_wait_usbsd(void);
static bool hook_ums_device_initilize(trampoline_state* state);
#endif

typedef struct {
    FSSALHandle *handle;
    volatile void *server_handle;
} WFSDeviceContext;

static WFSDeviceContext wfs_devices[2] = {{NULL, NULL}, {NULL, NULL}};
static volatile bool usb_handle_set = false;

static void hai_write_file_patch(trampoline_t_state *s){
    uint32_t *buffer = (uint32_t*)s->r[1];
    debug_printf("HAI WRITE COMPANION\n");
    if(hai_ctx.active && hai_getdev() == DEVTYPE_USB){
        hai_companion_add_offset(buffer, partition_offset);
    }
}

static int hai_umsBlkDevId_patch(int entry_id, char* umsdev_id, size_t size, int r3, int(*hai_param_add)(int, char*, size_t)){
    if(hai_ctx.active && hai_getdev() == DEVTYPE_USB){
        debug_printf("%s: Patching umsdev id to %016llX..\n", PLUGIN_NAME, *(u64*)umsdev_id);
        umsdev_id = hai_ctx.umsBlkDevID;
        size = sizeof(hai_ctx.umsBlkDevID);
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
typedef struct {
    bool attached;
    FSSALHandle *sd_handle;
    FSSALHandle *usb_handle;
    int umsDeviceCount;
    int noSDCount;
} SDContext;

static SDContext sd_ctx = {false, NULL, NULL, 0, 0};

bool wafel_usb_partition_wait_usbsd(void){
    return sd_ctx.umsDeviceCount > sd_ctx.noSDCount;
}

static bool hook_ums_device_initilize(trampoline_state* state){
    debug_printf("%s ums_device_initialize called device=%p\n", PLUGIN_NAME, (void*)state->r[0]);
    sd_ctx.umsDeviceCount++;
    return false;
}

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


FSSALHandle* usb_attach_hook(FSSALAttachDeviceArg *attach_arg, int r1, int r2, int r3, FSSALHandle* (*sal_attach)(FSSALAttachDeviceArg*)){
    if(attach_arg->params.device_type == DEVTYPE_SD){
        debug_printf("%s: Already SD device type, skipping attach hook\n", PLUGIN_NAME);
        return sal_attach(attach_arg);
    }
    
    u32 part_offset, part_size;
    bool has_fat = false;
    int res = read_usb_partition_from_mbr(attach_arg, &part_offset, &part_size, hai_ctx.active ? NULL : (u8*)hai_ctx.umsBlkDevID, &has_fat);

    FSSALHandle *sd_handle = NULL;
#ifdef MOUNT_SD
    if(res>=1) { // MBR detected
        if (has_fat && !sd_ctx.attached) {
            debug_printf("%s: MBR detected with FAT, attaching for SD\n", PLUGIN_NAME);
            sd_handle = clone_patch_attach_sd_hanlde(attach_arg);
            debug_printf("%s: Attached for SD, res: 0x%X\n", PLUGIN_NAME, sd_handle);
            if (sd_handle) sd_ctx.attached = true;
        }

        if (!has_fat)
            sd_ctx.noSDCount++;
    } else
        sd_ctx.noSDCount++;
#endif

    int wfs_slot = -1;
    if (res == 2) {
        for (int i = 0; i < 2; i++) {
            if (wfs_devices[i].handle == NULL) {
                wfs_slot = i;
                break;
            }
        }
    }

    if (wfs_slot == -1) {
        debug_printf("%s: No WFS detected or no free slots, creating dummy USB device\n", PLUGIN_NAME);
        patch_dummy_attach_arg(attach_arg);
    } else {
        if (wfs_slot == 0 && !hai_ctx.active) {
            hai_ctx.active = true;
            usb_handle_set = true;
        }
        patch_partition_attach_arg(attach_arg, wfs_slot, part_offset, part_size);
    } 
    
    debug_printf("%s: Attatching USB\n", PLUGIN_NAME);
    FSSALHandle *usb_handle = sal_attach(attach_arg);
    debug_printf("%s: Attached USB\n", PLUGIN_NAME);
    if(res==2 && wfs_slot != -1) {
        wfs_devices[wfs_slot].handle = usb_handle;
        wfs_devices[wfs_slot].server_handle = attach_arg->server_handle;
    }

#ifdef MOUNT_SD
    if (sd_handle) {
        sd_ctx.usb_handle = usb_handle;
        sd_ctx.sd_handle = sd_handle;
    }
#endif

    return usb_handle;
}

void usb_detach_hook(FSSALHandle *device_handle, int r1, int r2, int r3, void (*sal_detach)(FSSALHandle*)){
#ifdef MOUNT_SD
    if(sd_ctx.usb_handle == device_handle){
        debug_printf("%s: Detaching cloned handle pair: USB: %p SD: %p\n", PLUGIN_NAME, sd_ctx.usb_handle, sd_ctx.sd_handle);
        sal_detach(sd_ctx.sd_handle);
        sd_ctx.usb_handle = sd_ctx.sd_handle = NULL;
        sd_ctx.attached = false;
    }
#endif
    sal_detach(device_handle);
    if(device_handle == wfs_devices[0].handle){
        debug_printf("%s: Detached partition handle 0, deactivating partition patching for HAI\n", PLUGIN_NAME);
        hai_ctx.active = false;
        wfs_devices[0].handle = NULL;
        wfs_devices[0].server_handle = NULL;
    } else if (device_handle == wfs_devices[1].handle) {
        debug_printf("%s: Detached partition handle 1\n", PLUGIN_NAME);
        wfs_devices[1].handle = NULL;
        wfs_devices[1].server_handle = NULL;
    }
}


static void wfs_initDeviceParams_exit_hook(trampoline_state *regs){
    WFS_Device *wfs_device = (WFS_Device*)regs->r[5];
    FSSALDevice *sal_device = FSSAL_LookupDevice(wfs_device->handle);
    void *server_handle = sal_device->server_handle;
    debug_printf("wfs_initDeviceParams_exit_hook server_handle: %p\n", server_handle);
    if(usb_handle_set && (server_handle == wfs_devices[0].server_handle || server_handle == wfs_devices[1].server_handle)) {
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
