# wafel_usb_partition

This plugin checks the USB storage device for an MBR. If an MBR is found ~~it will be attached as sdcard~~ and the other three partition slots in the MBR will be checked. The NTFS partition with the highest start lba, will be attached as USB.

*won't attach the FAT32 for now since it causes problems with Aroma

## How to use

- Copy the `wafel_usb_partition.ipx` to `sd:/wiiu/ios_plugins` or `/storage_slc/sys/hax/ios_plugins`
- Create one FAT32 partition as the first partition on the USB device. Create a second partition, which will be used as USB.

## Building

```bash
export STROOPWAFEL_ROOT=/path/too/stroopwafel-repo
make
```

## Thanks

- [shinyquagsire23](https://github.com/shinyquagsire23)
