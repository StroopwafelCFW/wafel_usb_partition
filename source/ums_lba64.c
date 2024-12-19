#include <wafel/trampoline.h>
#include <wafel/utils.h>
#include <wafel/patch.h>

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

  //u8 *c = (u8*) &cmd;
  //debug_printf("scsi read: %02x\nflg: %02x\nlba: %02x %02x %02x %02x %02x %02x %02x %02x\nlen: %02x %02x %02x %02x\ngrp: %02x\nctl: %02x\n", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);

  return UmsTpMakeTransferRequest(ums_server, ums_tp, 2, lun, &cmd, sizeof(cmd), buf, buf_len, timeout, event);
}

int ums_read_hook(void *ums_server, void *ums_tp, u8 lun, u32 timeout, int r4, int r5, int r6, u32 *r7, int r8, int r9, int r10, int r11, 
                      ums_read_func *org_read, const void *lr, void *buf, u32 lba, u16 transfer_length, u32 buf_len, void *event) {
  int lba_hi = r7[0x5c/4];
  //debug_printf("ums read: lba_hi: %u, lba: %u, length: %u event: %p\n", lba_hi, lba, transfer_length, event);
  //*(int*)(ums_server + 0x134) = 1; // enables Tp Fsm Tracing
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

  return UmsTpMakeTransferRequest(ums_server, ums_tp, 1, lun, &cmd, sizeof(cmd), buf, buf_len, timeout, event);
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

static const char *states[] = {
  "NULL",
  "IDLE",
  "COMMAND",
  "TRANSFER",
  "STATUS",
  "DESTROY",
  "DESTROYED",
};

static const char *events[] = {
  "INVALID",
  "ENTRY",
  "EXIT",
  "INIT",
  "START",
  "COMMAND_BULK_DONE",
  "XFER_BULK_DONE",
  "STATUS_BULK_DONE",
  "IN_STALL_RECOVERY",
  "OUT_STALL_RECOVERY",
  "TIMEOUT",
  "DESTROY_REQUEST",
  "CONFIRM",
  "ERROR"
};

static void UmsTpFsmProcessState_hook(trampoline_state *regs){
  int state = regs->r[1];
  int event = regs->r[2];
  debug_printf("UmsTpFsmProcessState_hook: state: %s, event: %s\n", states[state], events[event]);
}

static void bulk_done_hook(trampoline_state *regs){
  u8 *arg= (void*) regs->r[4];

  debug_printf("XFER_BULK_DONE_hook: CBWCB: %02x %02x %02x %02x\n", arg[4], arg[5], arg[6], arg[7]);

}

static void UmsTpMakeTransferRequest_hook(trampoline_state *regs){
  u8 *cbwcb = (u8*) regs->r[12];
  if((void*)regs->stack[10] != cbwcb){
    debug_printf("WARNING: Wrong stack offset\n");
  }
  u32 cbwcb_len = regs->stack[11];
  debug_printf("UmsTpMakeTransferRequest: CBWCB len=%d\nCBWCB=", cbwcb_len);
  for(int i=0; i<cbwcb_len; i++){
    debug_printf("%02X ", cbwcb[i]);
  }
  debug_printf("\n");
}

void patch_ums_lba64(void) {
  trampoline_blreplace_with_regs(0x1077fbc4, ums_read_hook);

  /* DEBUG STUFF */
  //trampoline_blreplace_with_regs(0x1077fb34, ums_write_hook);
  //trampoline_blreplace_with_regs(0x1077fbfc, ums_sync_hook);

  //trampoline_hook_before(0x107827c4, UmsTpFsmProcessState_hook);
  //trampoline_hook_before(0x10782de4, bulk_done_hook);

  //trampoline_hook_before(0x10783778, UmsTpMakeTransferRequest_hook);

  //enable logging
  // ASM_PATCH_K(0x107f080c, "cmp r3, r3");
  // ASM_PATCH_K(0x107f07f8, "nop");
  // ASM_PATCH_K(0x107f0800, "nop");
}