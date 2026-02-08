#pragma once

#include <wafel/types.h>
#include "sal.h"

#define DEVTYPE_USB 17
#define DEVTYPE_SD 6

typedef struct {
    u32 offset;
    u32 size;
} HAI_PartitionInfo;

extern HAI_PartitionInfo hai_partition;

void patch_partition_attach_arg(FSSALAttachDeviceArg *attach_arg, int index, u32 offset, u32 size);