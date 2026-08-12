# Memory Architecture & Paging Strategy

## 1. System Constraints (STM32H7B0VB)
- **AXI SRAM**: 870KB total. (32KB reserved for VRAM, leaving ~840KB for guest RAM).
- **ITCM**: 64KB (Instruction Tightly Coupled Memory) - 0 wait states.
- **DTCM**: 128KB (Data Tightly Coupled Memory) - 0 wait states.
- **AHB SRAM**: Used as DMA buffers for SD card I/O.
- **NOR Flash**: Quad-SPI (QSPI), memory-mapped, read-only Execution-In-Place (XIP) capable.
- **Guest RAM Target**: Up to 8MB.

## 2. Page Directory & DTCM
To emulate an 8MB guest address space efficiently, we divide it into 4KB pages (totaling 2048 pages).
The memory management unit (MMU) of the emulator requires a Page Table to translate Guest Physical Addresses to Host Physical Addresses.

Given 2048 pages, a flat page table where each entry is 32-bits (4 bytes) requires exactly **8KB**.
**Decision**: This 8KB Page Table will be permanently pinned to the **DTCM**.
This guarantees 0-wait-state lookups during the emulator's execution loop.

### Page Table Entry (PTE) Format
A 32-bit PTE will contain:
- `Bits [31:12]`: Host Physical Address (or block index).
- `Bit [0]`: `VALID` (1 = accessible, 0 = page fault).
- `Bit [1]`: `LOCATION` (0 = NOR Flash XIP, 1 = AXI RAM).
- `Bit [2]`: `DIRTY` (1 = modified, needs writeback).
- `Bit [3]`: `SD_PAGED` (1 = page is swapped out to the SD card).

## 3. The Copy-On-Write (COW) Paging Strategy
At boot, the guest OS snapshot is stored entirely in the memory-mapped NOR Flash. 
All 2048 Page Table entries are initialized to point to their respective NOR Flash offsets, with `LOCATION = 0` and `DIRTY = 0`.

### Read Operations
1. CPU attempts to read a guest address.
2. Emulator fetches the PTE from DTCM.
3. If `LOCATION == 0`, the emulator reads directly from the memory-mapped NOR Flash (XIP). 
4. If `LOCATION == 1`, it reads from AXI RAM.
5. If `SD_PAGED == 1`, it triggers a page fault to swap the page back in from the SD card.

### Write Operations (COW)
1. CPU attempts to write to a guest address.
2. Emulator fetches the PTE.
3. If `LOCATION == 0` (pointing to read-only NOR flash):
   - A **Page Fault** is triggered.
   - The memory controller allocates a free 4KB page from the ~840KB AXI pool.
   - The 4KB chunk is copied from NOR Flash into the new AXI page.
   - The write operation is performed on the AXI page.
   - The PTE is updated: `Host Address` updated to AXI, `LOCATION = 1`, `DIRTY = 1`.

## 4. AXI Pool Management & SD Card Swapping
The ~840KB AXI pool can hold a maximum of 210 pages (4KB each). 
We will maintain a doubly-linked **LRU (Least Recently Used) list** in DTCM to track page hotness.

When a COW operation occurs and the AXI pool is full (all 210 pages are in use):
1. The memory controller evicts the coldest (tail) page from the LRU list.
2. If the evicted page is `DIRTY`, it is written out to the **Backend Pagefile** on the SD card using AHB DMA.
3. The evicted page's PTE is updated to `SD_PAGED = 1`.
4. The newly freed AXI page is given to the COW operation.

**Note**: To prevent blocking the CPU on SD writes, dirty page evictions should be pipelined via DMA using the AHB SRAM as a staging buffer.

## 5. Physical Memory Mapping (Linker Script)
To implement this architecture, we define the exact physical boundaries of the STM32H7B0VB in our linker script. Crucially, while the AXI RAM is 1024KB total, we reclaim unused RGB565 framebuffer space (reserving only 154KB for LUT8 mode), giving the emulator an expanded ~870KB AXI pool.
```ld
MEMORY {
  ITCM (rx)     : ORIGIN = 0x00000000, LENGTH = 64K
  DTCM (rwx)    : ORIGIN = 0x20000000, LENGTH = 128K
  AXI  (rwx)    : ORIGIN = 0x24000000, LENGTH = 870K   /* 1024K - 154K (LUT8 framebuffer) */
  INTFLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
  EXTFLASH (rx) : ORIGIN = 0x90000000, LENGTH = 64M
}
```
*Note: To ensure seamless integration with the `retro-go` framework, we mirror its `Makefile.common` variable naming conventions (`EXTFLASH_SIZE_MB`, `EXTFLASH_OFFSET`, `INTFLASH_BANK`).*

## 6. Page Table Implementation
The 8KB flat page table must strictly reside in DTCM to guarantee 0-wait-state lookups. This is enforced via GCC section attributes tied to the linker script.
```c
// 2048 entries of 32-bits (8MB / 4KB) = 8192 bytes.
// Forced into the DTCM via linker section.
uint32_t guest_page_table[2048] __attribute__((section(".dtcm_bss")));
```
