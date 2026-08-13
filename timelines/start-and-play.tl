# Open the preselected ROM, then keep pressing keys inside the guest.
#
# start-preselected-game.tl only launches the ROM and waits. A DOS game
# typically sits on a title screen until a key arrives, so proving the CPU is
# still alive past that screen needs input DELIVERED TO THE GUEST -- which is
# what these later presses are. A maps to Space and START to Enter (see
# pad_scancode in src/main.c).
4.0  press a
5.0  press a
9.0  press start
11.0 press a
14.0 press a
17.0 press start
20.0 press a
23.0 press a
26.0 press a
35.0 quit
