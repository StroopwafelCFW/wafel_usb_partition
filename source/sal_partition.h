#pragma once

#include <wafel/types.h>
#include "sal.h"

#define DEVTYPE_USB 17
#define DEVTYPE_SD 6

u32 get_partition_offset(void);

void patch_partition_attach_arg(FSSALAttachDeviceArg *attach_arg, int index, u32 offset, u32 size);