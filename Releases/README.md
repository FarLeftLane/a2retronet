# A2retroNET THIS IS A ALPHA TEST, backup your SD card first.  

This project is based on [A2Pico](https://github.com/oliverschmidt/a2pico).

A2retroNET implements a SmartPort mass storage controller with (up to) eight drives. The (currently) only supported disk image format is a ProDOS block image of up to 32MB, typically with the file extensions `.hdv`, `.po` or `.2mg`. 

## A2retroNET_Cache_v1.6b.uf2

This firmware is a test of a set of performance upgrades.  Block cache, read ahead, write behind and pseudo DMA.  


To update your a2pico:

Download the Firmware: Download the desired firmware file A2retroNET_Cache_v1.6b.uf2

# With your Apple II OFF:

Enter BOOTSEL Mode:
Locate the white BOOTSEL button on your Pico board (usually on the front, near the USB port).
Press and hold the BOOTSEL button.
While still holding the button, connect the Pico to your computer using a micro USB cable.

Release the BOOTSEL button after a few seconds.

Access the Drive: Your computer should recognize the Pico as a removable mass storage device named RPI-RP2. A file explorer window may open automatically.

Flash the Pico: Drag and drop the downloaded .uf2 file onto the RPI-RP2 volume.

Reboot: After the file copy is complete, the Pico will automatically reboot. The RPI-RP2 drive will disappear from your computer, and the new firmware will be running on the board. 
