# The OSK's disk menu: the "A:" cap (end of the third row) lists the other
# .dsk images in the title's folder and A mounts the selected one in place of
# the running title's disk.
#
# Needs a card with MORE THAN ONE image and one preselected, e.g.
#   python3 scripts/sdcard.py --image build/run/sd-swap.img --core dos.bin \
#       --rom references/.../ALLEYCAT.dsk --rom .../OTHER.dsk --preselect ALLEYCAT.dsk
#
# The keyboard opens on 'A' (row 2, cap 1); three LEFTs reach "A:" (Ctl,
# then the wrap to X, then A:). The menu opens on the mounted image, DOWN picks the next one, A inserts
# it and closes everything with a toast. What to watch: x86_osk_mode (3 while
# the menu is up), dsk_swaps (1 after the insert), dsk_mounted (still 1) and
# x86_disk_backend_reads (the new image being read when the guest next asks).
4.0  press a
5.0  press a
10.0 press game
11.5 press left
12.5 press left
13.5 press left
14.0 screenshot 01-cursor-on-a.png
15.0 press a
17.0 screenshot 02-disk-menu.png
18.0 press down
19.0 screenshot 03-next-picked.png
20.0 press a
21.0 screenshot 04-swapped-toast.png
26.0 quit
