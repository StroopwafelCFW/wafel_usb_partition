# wafel_usb_partition

This plugin checks the USB storage device for an MBR. If an MBR is found, the first partition will be ignored and the other three partition slots in the MBR will be checked. The NTFS partition with the highest start lba, will be attached as USB.

### sd emulation

the SD version of the plugin will attach the first usb device as SD card. So the first partition will be seen as SD card by the Wii U

## How to use

- Copy the `5usbpart.ipx` or `5upartsd.ipx` (for sd emulation) to `sd:/wiiu/ios_plugins` or `/storage_slc/sys/hax/ios_plugins`
- Create one FAT32 partition as the first partition on the USB device. Create a second partition, which will be used as USB.

## Building

```bash
export STROOPWAFEL_ROOT=/path/too/stroopwafel-repo
make
```

## Thanks

- [shinyquagsire23](https://github.com/shinyquagsire23)
