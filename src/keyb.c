#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>

#include "ubee512.h"
#include "audio.h"
#include "keyb.h"
#include "keystd.h"
#include "keytc.h"
#include "support.h"
#include "fdc.h"
#include "gui.h"
#include "joystick.h"
#include "osd.h"
#include "mouse.h"
#include "tape.h"
#include "tapfile.h"
#include "video.h"
#include "z80debug.h"

//==============================================================================
// structures and variables
//==============================================================================
static uint64_t ticks_repeat;

static int cmd_last = -1;

static int func_key_down;
static int keys_context;
static int joystick_keys_sel;

static SDLKey cmd_key;

extern emu_t emu;
extern video_t video;
extern model_t modelx;
extern modio_t modio;
extern tape_t tape;
extern joystick_t joystick;
extern fdc_t fdc;

//==============================================================================
// Keyboard initialise
//
//   pass: void
// return: int                          0
//==============================================================================
int keyb_init (void)
{
 keyb_set_unicode(0);

 if (modelx.tckeys)
    {
     if (modelx.lpen)
        return keytc_init() | keystd_init();
     else
        return keytc_init();
    }
 else
    return keystd_init();
}

//==============================================================================
// Keyboard de-initialise
//
//   pass: void
// return: int                          0
//==============================================================================
int keyb_deinit (void)
{
 if (modelx.tckeys)
    {
     if (modelx.lpen)
        return keytc_deinit() | keystd_deinit();
     else
        return keytc_deinit();
    }
 else
    return keystd_deinit();
}

//==============================================================================
// Keyboard reset
//
//   pass: void
// return: int                          0
//==============================================================================
int keyb_reset (void)
{
 if (modelx.tckeys)
    {
     if (modelx.lpen)
        return keytc_reset() | keystd_reset();
     else
        return keytc_reset();
    }
 else
    return keystd_reset();
}

//==============================================================================
// Set unicode on or off.
//
// Unicode is used by the OSD, this also makes use of SDL's key repeating.
// As unicode has some overhead it should be disabled when not needed.
//
//   pass: int enable                   1 to enable, 0 to disable
// return: void
//==============================================================================
void keyb_set_unicode (int enable)
{
 SDL_EnableUNICODE(enable);
 if (enable)
    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
 else
    SDL_EnableKeyRepeat(0, 0);
}

//==============================================================================
// Emulator commands activated using EMUKEY or via the joystick.
//
//   pass: int cmd                      command number
//         int p                        parameter
// return: void
//==============================================================================
void keyb_emu_command (int cmd, int p)
{
 cmd_last = cmd;  // will need this value for command repeating

 switch (cmd)
    {
     case EMU_CMD_DUMP      :
     case EMU_CMD_DUMP_N1   :
     case EMU_CMD_DUMP_N2   :
     case EMU_CMD_DUMP_B1   :
     case EMU_CMD_DUMP_B2   :
     case EMU_CMD_DUMP_REP  :
     case EMU_CMD_DUMPREGS  :
     case EMU_CMD_DBGOFF    :
     case EMU_CMD_DBGON     :
     case EMU_CMD_DBGTRACE  :
     case EMU_CMD_DBGSTEP01 :
     case EMU_CMD_DBGSTEP10 :
     case EMU_CMD_DBGSTEP20 :
     case EMU_CMD_DASML     :
     case EMU_CMD_PAUSE     : z80debug_command(cmd, 1);
                              break;

     case EMU_CMD_TAPEREW   : tape_command(cmd);
                              tapfile_command(cmd);
                              break;

     case EMU_CMD_JOYSTICK  : joystick_command(cmd, p);
                              break;

     case EMU_CMD_MUTE      :
     case EMU_CMD_VOLUMEI   :
     case EMU_CMD_VOLUMED   : audio_command(cmd);
                              break;

     case EMU_CMD_FULLSCR   :
     case EMU_CMD_SCREENI   :
     case EMU_CMD_SCREEND   :
     case EMU_CMD_VIDSIZE1  :
     case EMU_CMD_GL_FILTER : video_command(cmd, p);
                              break;

     case EMU_CMD_MWHEEL    : gui_command(cmd);
                              break;

     case EMU_CMD_CONSOLE   : console_command(cmd);
                              break;

     case EMU_CMD_MOUSE     : mouse_command(cmd);
                              break;
    }

 gui_status_update();
}

//==============================================================================
// Emulator command repeat start.
//
// Saves the current system time to be used later for command repeats.
//
//   pass: void
//         void
// return: void
//==============================================================================
void keyb_repeat_start (void)
{
 ticks_repeat = time_get_ms() + emu.cmd_repeat1;
}

//==============================================================================
// Emulator command repeat stop.
//
// Terminates the repeated command.
//
//   pass: void
//         void
// return: void
//==============================================================================
void keyb_repeat_stop (void)
{
 cmd_last = -1;
}

//==============================================================================
// Emulator command repeating.
//
// Called on a regular basis (between frames) from the ubee512.c module.
// This function handles command repeating.
//
//   pass: void
//         void
// return: void
//==============================================================================
void keyb_update (void)
{
 if (cmd_last == -1)
    return;
 else
    {
     if (time_get_ms() >= ticks_repeat)
        {
         keyb_emu_command(cmd_last, 0);
         ticks_repeat += emu.cmd_repeat2;
        }
    }
}

//==============================================================================
// key down event handler.
//
//   pass: void
// return: void
//==============================================================================
void keyb_keydown_event (void)
{
 SDLKey key;
 int p;

 key = emu.event.key.keysym.sym;

 fdc.nodisk = 0;        // any key clears this state.

 if (emu.display_context != EMU_OSD_CONTEXT)
    {
     // check for the emulator's exit key
     if (key == SDLK_END)
        {
         keys_context |= 0x00000001;
         emu.quit = 1;
         if (emu.exit_check)
            osd_set_dialogue(DIALOGUE_EXIT);
         return;
        }

    // check for the emulator's reset key
     if (key == SDLK_PAGEDOWN)
        {
         if (! func_key_down)
            {
             keys_context |= 0x00000002;
             emu.reset = EMU_RST_RESET_CON;
             if (emu.keyesc || emu.keym)
                {
                 emu.reset = EMU_RST_RESET_NOW;
                 emu.keyesc = 0;
                 emu.keym = 0;
                }
             else
                osd_set_dialogue(DIALOGUE_RESET);
             return;
            }
         else
            {
             keys_context |= 0x00000002;
             emu.reset = EMU_RST_POWERCYC_CON;
             emu.keyesc = 0;
             emu.keym = 0;
             osd_set_dialogue(DIALOGUE_POWERCYC);
             return;
            }
        }
    }

// joystick hot keys EMUKEY+J <K>
 if (joystick_keys_sel)
    {
     joystick_keys_sel = 0;
     p = 25 - (SDLK_z - key);
     if ((p >= 0) && (p < 26))
        keyb_emu_command(EMU_CMD_JOYSTICK, p);
     if (key == SDLK_0)
        keyb_emu_command(EMU_CMD_JOYSTICK, 26);  // disable mbee/kbd joystick
     if (key == SDLK_1)
        keyb_emu_command(EMU_CMD_JOYSTICK, 27);  // enable mbee joystick
     return;
    }

 if ((key == SDLK_HOME) || (modelx.lpen && ((key == SDLK_LALT) || (key == SDLK_RALT))))
    {
     func_key_down = 1;
     return;
    }

 if (func_key_down)
    {
     cmd_key = key;
     keyb_repeat_start();

     switch (key)
        {
         case SDLK_d            : keyb_emu_command(EMU_CMD_DUMP, 0); break;
         case SDLK_1            : keyb_emu_command(EMU_CMD_DUMP_N1, 0); break;
         case SDLK_2            : keyb_emu_command(EMU_CMD_DUMP_N2, 0); break;
         case SDLK_3            : keyb_emu_command(EMU_CMD_DUMP_B1, 0); break;
         case SDLK_4            : keyb_emu_command(EMU_CMD_DUMP_B2, 0); break;
         case SDLK_5            : keyb_emu_command(EMU_CMD_DUMP_REP, 0); break;
         case SDLK_r            : keyb_emu_command(EMU_CMD_DUMPREGS, 0); break;
         case SDLK_EQUALS       : keyb_emu_command(EMU_CMD_DBGON, 0); break;
         case SDLK_MINUS        : keyb_emu_command(EMU_CMD_DBGOFF, 0); break;
         case SDLK_BACKSLASH    : keyb_emu_command(EMU_CMD_DBGTRACE, 0); break;
         case SDLK_BACKSPACE    : keyb_emu_command(EMU_CMD_DBGSTEP01, 0); break;
         case SDLK_LEFTBRACKET  : keyb_emu_command(EMU_CMD_DBGSTEP10, 0); break;
         case SDLK_RIGHTBRACKET : keyb_emu_command(EMU_CMD_DBGSTEP20, 0); break;
         case SDLK_l            : keyb_emu_command(EMU_CMD_DASML, 0); break;
         case SDLK_p            : keyb_emu_command(EMU_CMD_PAUSE, 0); break;
         case SDLK_RETURN       : keyb_emu_command(EMU_CMD_FULLSCR, 0); break;
         case SDLK_t            : keyb_emu_command(EMU_CMD_TAPEREW, 0); break;
         case SDLK_s            : keyb_emu_command(EMU_CMD_MUTE, 0); break;
         case SDLK_UP           : keyb_emu_command(EMU_CMD_VOLUMEI, 0); break;
         case SDLK_DOWN         : keyb_emu_command(EMU_CMD_VOLUMED, 0); break;
         case SDLK_f            : keyb_emu_command(EMU_CMD_GL_FILTER, 0); break;
         case SDLK_KP_PERIOD    : keyb_emu_command(EMU_CMD_VIDSIZE1, 0); break;
         case SDLK_KP1          : keyb_emu_command(EMU_CMD_VIDSIZE1, 1); break;
         case SDLK_KP2          : keyb_emu_command(EMU_CMD_VIDSIZE1, 2); break;
         case SDLK_KP3          : keyb_emu_command(EMU_CMD_VIDSIZE1, 3); break;
         case SDLK_KP4          : keyb_emu_command(EMU_CMD_VIDSIZE1, 4); break;
         case SDLK_KP5          : keyb_emu_command(EMU_CMD_VIDSIZE1, 5); break;
         case SDLK_KP6          : keyb_emu_command(EMU_CMD_VIDSIZE1, 6); break;
         case SDLK_KP7          : keyb_emu_command(EMU_CMD_VIDSIZE1, 7); break;
         case SDLK_KP8          : keyb_emu_command(EMU_CMD_VIDSIZE1, 8); break;
         case SDLK_KP9          : keyb_emu_command(EMU_CMD_VIDSIZE1, 9); break;
         case SDLK_w            : keyb_emu_command(EMU_CMD_MWHEEL, 0); break;
         case SDLK_m            : keyb_emu_command(EMU_CMD_MOUSE, 0); break;
         case SDLK_c            : keyb_emu_command(EMU_CMD_CONSOLE, 0);
                                  keyb_repeat_stop();
                                  func_key_down = 0;
                                  break;
         case SDLK_j            : joystick_keys_sel = 1;
                                  keyb_repeat_stop();
                                  break;
         default                : break;
        }
     return;
    }

 // if OSD is active then keys handled by OSD then exit
 if ((emu.display_context == EMU_OSD_CONTEXT) && (emu.osd_focus))
    {
     osd_keydown_event();
     return;
    }

 // if 256TC/Teleterm keys are required
 if (modelx.tckeys)
    keytc_keydown_event();

 // if CRTC 6545 (light pen keys) are required
 if (modelx.lpen)
    keystd_keydown_event();
}

//==============================================================================
// key up event handler.
//
//   pass: void
// return: void
//==============================================================================
void keyb_keyup_event (void)
{
 SDLKey key;

 key = emu.event.key.keysym.sym;

 // handle dedicated emulator control keys
 if (key == SDLK_PAGEDOWN)
    {
     if (func_key_down)
        keys_context &= ~0x00000002;
     else
        keys_context &= ~0x00000002;
    }
 if (key == SDLK_END)
    keys_context &= ~0x00000001;
 if (keys_context)
    return;

 // handle the emulator's EMUKEY control key
 if ((key == SDLK_HOME) || (modelx.lpen && ((key == SDLK_LALT) || (key == SDLK_RALT))))
    {
     func_key_down = 0;
     return;
    }

 if (cmd_key == key)
    keyb_repeat_stop();

// Ignore a released key if EMUKEY is active.  This needs to be implemented
// in such a way to avoid the emulated KB seeing the key, for now the OSD
// should be given the context to minimise the affects.
//
// if (func_key_down)   // FIX ME
//    return;

 // if OSD is active and in focus then keys handled by OSD and exit
 if ((emu.display_context == EMU_OSD_CONTEXT) && (emu.osd_focus))
    {
     osd_keyup_event();
     return;
    }

 // if 256TC/Teleterm keys are required
 if (modelx.tckeys)
    keytc_keyup_event();

 // if CRTC 6545 (light pen keys) are required
 if (modelx.lpen)
    keystd_keyup_event();
}

//==============================================================================
// Force a character to be returned.
//
// This is useful for placing the boot code into various modes on start-up
// such as Machine monitor (M), Floppy drive on a HDD system (F) mode by
// forcing the 'M' key (0x0d) to be returned or for pasting.
//
//   pass: int scan                     scan code 0-63
//   pass: int counts                   number of times to be forced
// return: void
//==============================================================================
void keyb_force (int scan, int counts)
{
 if (modelx.tckeys)
    keytc_force(scan, counts);
 else
    keystd_force(scan, counts);
}

//==============================================================================
// Force none.
//
// Forces no scan matches for counts
//
//   pass: int counts                   number of times to be forced
// return: void
//==============================================================================
void keyb_force_none (int counts)
{
 if (modelx.tckeys)
    keytc_force_none(counts);
 else
    keystd_force_none(counts);
}
