# Device Emulation & FatFs High-Level Emulation (HLE)

## 1. Minimal PC Devices
To support Windows Protected Mode and standard DOS applications, we must implement a minimal subset of PC hardware in Thumb v2 assembly:
- **PIT (Programmable Interval Timer)**: Required for OS scheduling.
- **PIC (Programmable Interrupt Controller)**: Required for hardware interrupt routing.
- **Keyboard (PS/2)**: Interfaced via Retro-Go's input ABI.
- **VGA**: Requires specialized memory mapping.

## 2. VGA Memory Management
The STM32H7B0VB has a ~154KB Framebuffer. 
Standard VGA text and CGA graphics require 32KB of VRAM. This 32KB is permanently allocated from the AXI SRAM pool.

### EGA 256KB Mode
When a guest requests EGA high-resolution graphics (which requires 256KB of VRAM), we cannot satisfy it with the base 32KB allocation.
- We will dynamically borrow the additional **224KB** from the AXI guest RAM pool (~840KB).
- This temporarily reduces the available physical RAM for guest pages, causing more aggressive swapping to the SD card.
- However, games utilizing EGA 256KB typically require less raw guest RAM, making this dynamic borrowing acceptable.
- VGA accesses will be trapped and translated linearly to this memory block, which is periodically blitted to the Retro-Go framebuffer via DMA2D or CPU depending on format.

## 3. Storage & FatFs High-Level Emulation (HLE)
Emulating a raw IDE controller (INT 13h) and forcing the guest OS to run its own FAT driver results in double-caching (the guest caches sectors in its RAM, and Retro-Go/FatFs caches them again), destroying performance.

To circumvent this, we will use **High-Level Emulation (HLE) for disk I/O**.

### Implementation Strategy
1. **Intercept DOS Interrupts**: We will hook INT 21h (DOS API) at the emulator level before the x86 CPU executes the interrupt.
2. **Translate API Calls**: When the guest requests a file open/read/write, we translate the INT 21h registers into direct `f_open`, `f_read`, `f_write` calls to the host's **FatFs** library.
3. **Bypass Guest OS Layers**: The guest OS's block layer and FAT driver are entirely bypassed. Data is DMA'd directly from the SD card into the guest's memory buffer in AXI RAM.

### Backend Pagefile Priority
Because guest disk I/O is routed through our HLE layer, we can strictly control SD card bandwidth. 
We will implement an I/O arbiter that prioritizes reads/writes to our **Backend Pagefile** (used for COW memory swapping) over standard guest file I/O. This ensures that memory page faults are resolved with minimum latency, preventing CPU starvation.

## 4. Speaker and Soundblaster 16 (HLE)
In planning still. Reference Linux kernel driver for sblaster16: https://raw.githubusercontent.com/torvalds/linux/refs/heads/master/sound/isa/sb/sb16.c 
