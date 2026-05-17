---
name: upload
description: Compile and upload firmware to the M5AtomS3 device. Use this skill whenever the user says "upload", "flash", "upload to device", "deploy firmware", "push to device", or anything that involves compiling and sending code to the hardware. Also trigger when the user mentions the device being out of date or wanting to test new code on the device. Pass --erase if the user mentions the device is stuck in a boot loop or needs a full wipe.
---

Run the flash script from the repo root. The script handles everything: compile, find the USB port, retry on failure, and notify when safe to unplug.

```bash
cd /Users/apollitt/Documents/GitHub/dmx-fire
scripts/flash.sh
```

If the user mentions `--erase`, a boot loop, or a bricked device:

```bash
cd /Users/apollitt/Documents/GitHub/dmx-fire
scripts/flash.sh --erase
```

The script prints loud banners during upload — do not interrupt it. When it prints "SAFE TO UNPLUG", the upload is complete.
