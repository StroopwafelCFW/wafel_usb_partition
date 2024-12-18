#include <wafel/trampoline.h>

int (*UmsTpMakeTransferRequest)
              (void *ums_server, void *ums_tp, u32 endpoint,u8 bCBWLUN,void *CBWCB,
              size_t bCBWCBLength, void *xfer_buf, u32 bytes_to_transfer, u32 timeout, void *local_event
              ) = (void*)0x10783730;

typedef int ums_read_func(void *ums_server, void *ums_tp, u8 lun, u32 timeout, void *buf, u32 lba, u16 transfer_length, u32 buf_len, void *event);
typedef ums_read_func ums_write_func;
typedef int ums_sync_func(void *ums_server, void *ums_tp, u8 lun, u32 timeout, u32 lba, u16 num_blocks, void *event);

struct rw16_cmd {
  u8 opcode;
  u8 protect: 3;
  u8 DPO: 1;
  u8 FUA: 1;
  u8 RARC: 1;
  u8 obsolete: 1;
  u8 dld2: 1;
  u32 lba_hi;
  u32 lba_lo;
  u32 transfer_length;
  u8 dld1: 1;
  u8 dld0: 1;
  u8 group_number: 6;
  u8 control;
} __attribute__((packed)) ALIGNED(4) typedef rw16_cmd;

_Static_assert(sizeof(rw16_cmd) == 16, "read16_cmd size must be 16!");

static int scsi_read16(void *ums_server, void *ums_tp, u8 lun, u32 timeout, void *buf, u32 lba_hi, u32 lba, u16 transfer_length, u32 buf_len, void *event){
  rw16_cmd cmd = {
    .opcode = 0x88,
    .lba_hi = lba_hi,
    .lba_lo = lba,
    .transfer_length = transfer_length,    
  };

  return UmsTpMakeTransferRequest(ums_server, ums_tp, 2, lun, &cmd, sizeof(cmd), buf, transfer_length, timeout, event);
}

int ums_read_hook(void *ums_server, void *ums_tp, u8 lun, u32 timeout, int r4, int r5, int r6, u32 *r7, int r8, int r9, int r10, int r11, 
                      ums_read_func *org_read, const void *lr, void *buf, u32 lba, u16 transfer_length, u32 buf_len, void *event) {
  int lba_hi = r7[0x5c/4];
  if(lba_hi)
    return scsi_read16(ums_server, ums_tp, lun, timeout, buf, lba_hi, lba, transfer_length, buf_len, event);

  return org_read(ums_server, ums_tp, lun, timeout, buf, lba, transfer_length, buf_len, event);
}


static int scsi_write16(void *ums_server, void *ums_tp, u8 lun, u32 timeout, void *buf, u32 lba_hi, u32 lba, u16 transfer_length, u32 buf_len, void *event){
  rw16_cmd cmd = {
    .opcode = 0x8a,
    .lba_hi = lba_hi,
    .lba_lo = lba,
    .transfer_length = transfer_length,    
  };

  return UmsTpMakeTransferRequest(ums_server, ums_tp, 1, lun, &cmd, sizeof(cmd), buf, transfer_length, timeout, event);
}

int ums_write_hook(void *ums_server, void *ums_tp, u8 lun, u32 timeout, int r4, int r5, int r6, u32 *r7, int r8, int r9, int r10, int r11, 
                      ums_write_func *org_write, const void *lr, void *buf, u32 lba, u16 transfer_length, u32 buf_len, void *event) {
  int lba_hi = r7[0x5c/4];
  if(lba_hi)
    return scsi_write16(ums_server, ums_tp, lun, timeout, buf, lba_hi, lba, transfer_length, buf_len, event);

  return org_write(ums_server, ums_tp, lun, timeout, buf, lba, transfer_length, buf_len, event);
}


struct sync16_cmd {
  u8 opcode;
  u8 reserved: 5;
  u8 obsolete: 1;
  u8 immed: 1;
  u8 obsolete2: 1;
  u32 lba_hi;
  u32 lba_lo;
  u32 number_blocks;
  u8 reserved2: 2;
  u8 group_number: 6;
  u8 control;
} __attribute__((packed)) ALIGNED(4) typedef sync16_cmd;

_Static_assert(sizeof(sync16_cmd) == 16, "sync16_cmd size must be 16!");

int scsi_sync_cache16(void *ums_server, void *ums_tp, u8 lun, u32 timeout, u32 lba_hi, u32 lba,  u16 num_blocks, void *event){
  sync16_cmd cmd = {
    .opcode = 0x91,
    .lba_hi = lba_hi,
    .lba_lo = lba,
    .number_blocks = num_blocks,    
  };

  return UmsTpMakeTransferRequest(ums_server, ums_tp, 2, lun, &cmd, sizeof(cmd), NULL, 0, timeout, event);
}

int ums_sync_hook(void *ums_server, void *ums_tp, u8 lun, u32 timeout, int r4, int r5, int r6, u32 *r7, int r8, int r9, int r10, int r11, 
                      ums_sync_func *org_sync, const void *lr, u32 lba, u16 num_blocks, void *event) {
  int lba_hi = r7[0x5c/4];
  if(lba_hi)
    return scsi_sync_cache16(ums_server, ums_tp, lun, timeout, lba_hi, lba, num_blocks, event);
  
  return org_sync(ums_server, ums_tp, lun, timeout, lba, num_blocks, event);
}




void patch_ums_lba64(void) {
  trampoline_blreplace_with_regs(0x1077fbc4, ums_read_hook);
  trampoline_blreplace_with_regs(0x1077fb34, ums_write_hook);
  trampoline_blreplace_with_regs(0x1077fbfc, ums_sync_hook);
}