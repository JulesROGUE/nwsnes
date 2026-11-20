/* nwsnes - order of the code inside the app.
 *
 * The app is linked in two steps: this script is used for the first one (a
 * relocatable link, the .nwa file); the installer (nwlink) does the second
 * one on the user's computer and simply appends our .text. All this script
 * does is put the functions in the order of core/hot.ld, aligned on a cache
 * line, so that the code that runs together shares the instruction cache. */
SECTIONS {
  .text : ALIGN(32) {
    INCLUDE core/hot.ld
    *(.text .text.*)
  }
}
