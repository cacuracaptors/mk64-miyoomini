/* savestate.ld -- added to the default link (-Wl,-T,port/ld/savestate.ld; INSERT keeps
   the rest of the default script). It splits the program's writable memory in three:

     .nosave_data   (before .data)  the port layer, the renderer and the ROM data objects
     .data .bss                     the game: what a save state holds (__mk_state_start..end)
     .nosave_bss    (after .bss)    the same objects' zeroed variables, the copies of libc's
                                    variables (.dynbss: stdout...) and anything a file puts in
                                    a .bss.nosave* section (the audio "ROM" buffers, main.c)

   The ROM data (kart and menu textures, staff ghosts...) and the audio "ROM" buffers are
   rebuilt the same at every start, so a state leaves them out (__rom_*: their checksum is
   compared at every save, see port/savestate_miyoo.c). The port layer and the renderer keep
   their own state across a load (screen, sound output, buttons, textures, threads). */
SECTIONS
{
  .nosave_data :
  {
    __nosave_data_start = .;
    */port/*.o(.data .data.*)
    */src/gfx/*.o(.data .data.*)
    __rom_data_start = .;
    test.[o](.data .data.*)
    data_segment2.[o](.data .data.*)
    */data/textures_0a.o(.data .data.*)
    */data/other_textures.o(.data .data.*)
    */data/texture_tkmk00.o(.data .data.*)
    */startup_logo.mio0.o(.data .data.*)
    */ceremony_data.mio0.o(.data .data.*)
    __rom_data_end = .;
    *(.data.nosave .data.nosave.*)
    . = ALIGN(4096);
    __nosave_data_end = .;
    __mk_state_start = .;
  }
}
INSERT BEFORE .data;

SECTIONS
{
  .nosave_bss (NOLOAD) :
  {
    . = ALIGN(4096);
    __mk_state_end = .;
    __nosave_bss_start = .;
    *(.dynbss)
    */port/*.o(.bss .bss.* COMMON)
    */src/gfx/*.o(.bss .bss.* COMMON)
    test.[o](.bss .bss.* COMMON)
    data_segment2.[o](.bss .bss.* COMMON)
    . = ALIGN(4096);
    __rom_bss_start = .;
    *(.bss.nosave.rom .bss.nosave.rom.*)
    __rom_bss_end = .;
    *(.bss.nosave .bss.nosave.*)
    __nosave_bss_end = .;
  }
}
INSERT AFTER .bss;
