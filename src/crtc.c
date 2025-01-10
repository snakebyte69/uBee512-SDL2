#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "ubee512.h"
#include "z80api.h"
#include "macros.h"
#include "support.h"
#include "crtc.h"
#include "keystd.h"
#include "vdu.h"
#include "video.h"

//==============================================================================
// structures and variables
//==============================================================================
static void crtc_calc_vsync_freq (void);
int crtc_update_cursor (void);

crtc_t crtc =
{
 .video = 1,
 .hdisp = 80,
 .vdisp = 25,
 .scans_per_row = 11,
 .vblank_method = 0,
 .monitor = 0,
 .std_col_type = 1,
 .flashrate = 4,
};

static double vsync_freq;

static int cur_blink_rate_t1r32;
static int cur_blink_rate_t1r16;
static int cur_blink_rate_c1r32;
static int cur_blink_rate_c1r16;

static int cur_blink_last;
static int cur_blink;
static int cur_mode;
static int cur_pos;

static int flashvideo_last;

static int crtc_regs_data[32];
static int vblank_divval;
static int vblank_cmpval;

static int htot;
static int vtot;
static int vtot_adj;
static int cur_start;
static int cur_end;
static int lpen;
static int reg;

static int mem_addr;
static int redraw;

#ifdef MINGW
#else
struct timeval tod_x;
#endif

static char *crtc_regs_names[] =
{
 "Horiz Total-1",
 "Horiz Displayed",
 "Horiz Sync Position",
 "VSYSNC, HSYNC Widths",
 "Vert Total-1",
 "Vert Total Adjust",
 "Vert Displayed",
 "Vert Sync Position",
 "Mode Control",
 "Scan Lines-1",
 "Cursor Start",
 "Cursor End",
 "Display Start Addr (H)",
 "Display Start Addr (L)",
 "Cursor Position (H)",
 "Cursor Position (L)",
 "Light Pen Reg (H)",
 "Light Pen Reg (L)",
 "Update Address Reg (H)",
 "Update Address Reg (L)"
 };

extern SDL_Surface *screen;
extern emu_t emu;
extern model_t modelx;
extern modio_t modio;
extern vdu_t vdu;
extern video_t video;


//==============================================================================
// CRTC Change Video
//
//
//   pass: void
// return: int                  0 if success, -1 if error
//==============================================================================
static int crtc_videochange (void)
{
 int crt_w;
 int crt_h;

 crt_w = crtc.hdisp * 8;
 crt_h = crtc.vdisp * crtc.scans_per_row;

 if (crt_w == 0 || crt_h == 0 || 
     crt_w > 720 || crt_h > 600)
    return -1;
 /*
  * For programs running in 40 column mode (such as Videotex),
  * the aspect ratio is forced to 1 as it looks better.
  */
 video_configure((crtc.hdisp < 50) ? 1 : video.aspect);
 vdu_configure(video.yscale);
 video_create_surface(crt_w, crt_h * video.yscale);

 crtc_set_redraw();
 crtc_redraw();
 video_render();

 crtc.resized = 0;          // clear the resized flag

 return 0;
}

//==============================================================================
// CRTC Initialise
//
//   pass: void
// return: int                  0 if sucess, -1 if error
//==============================================================================
int crtc_init (void)
{
 return 0;
}

//==============================================================================
// CRTC de-initialise
//
//   pass: void
// return: int                  0
//==============================================================================
int crtc_deinit (void)
{
 return 0;
}

//==============================================================================
// CRTC reset
//
//   pass: void
// return: int                  0
//==============================================================================
int crtc_reset (void)
{
 reg = 0;

 return 0;
}

//==============================================================================
// CRTC vblank status
//
// The Vertical blanking status is generated from the Z80 clock cycles that
// have elapsed or the host timer depending on the mode required.
//
//   pass: void
// return: int                  vblank status (in bit 7)
//==============================================================================
int crtc_vblank (void)
{
 uint64_t cycles_now;

 if (crtc.vblank_method == 0)
    {
     cycles_now = z80api_get_tstates();
     if ((cycles_now % vblank_divval) < vblank_cmpval)
        return B8(10000000);
    }
 else
    {
     if ((time_get_ms() / 10) & 1)      // div 10mS (100Hz)
        return B8(10000000);            // return true at a 50Hz rate
    }

 return 0;
}

//==============================================================================
// CRTC status
//
//   pass: uint16_t port
//         struct z80_port_read *port_s
// return: uint16_t             status
//==============================================================================
uint16_t crtc_status_r (uint16_t port, struct z80_port_read *port_s)
{
 int status = 0;

 crtc.update_strobe = B8(10000000);

 if (modelx.lpen && ! crtc.lpen_valid)
    keystd_checkall();

 if (modelx.lpen && crtc.lpen_valid)     // NB: this is not an else because
    status |= 0x40;                     // keystd_checkall might set lpen_valid

 if (crtc_vblank())
    status |= 0x20;

 if (modio.crtc)
    log_port_1("crtc_status_r", "status", port, status);

 return crtc.update_strobe | status;
}

//==============================================================================
// CRTC Light Pen
//
// Called from keystd_handler() and keystd_checkall() in keystd.c when a key
// is detected as pressed and sets lpen valid bit.
//
// If the passed 
//
//   pass: int addr
// return: void
//==============================================================================
void crtc_lpen (int addr)
{
 if (! crtc.lpen_valid)
    {
     crtc.lpen_valid = 1;
     lpen = addr;
     if (modio.crtc)
        log_data_1("crtc_lpen", "addr", addr);
    }
}

//==============================================================================
// Set CRTC reg address - Port function
//
//   pass: uint16_t port
//         uint8_t data
//         struct z80_port_write *port_s
// return: void
//==============================================================================
void crtc_address_w (uint16_t port, uint8_t data, struct z80_port_write *port_s)
{
 reg = data & 0x1F;

 if (modio.crtc)
    log_port_1("crtc_address_w", "data", port, data);
}

//==============================================================================
// Read CRTC register data - Port function
//
//   pass: uint16_t port
//         struct z80_port_read *port_s
// return: uint16_t register    register data
//==============================================================================
uint16_t crtc_data_r (uint16_t port, struct z80_port_read *port_s)
{
 uint16_t val;

 switch (reg)
    {
     case CRTC_CUR_POS_H:       // R14
        val = (cur_pos >> 8) & 0x3F;
        break;
     case CRTC_CUR_POS_L:       // R15
        val = cur_pos & 0xFF;
        break;

     case CRTC_LPEN_H:          // R16
        crtc.lpen_valid = 0;
        val = (lpen >> 8) & 0x3F;
        break;
     case CRTC_LPEN_L:          // R17
        crtc.lpen_valid = 0;
        val = lpen & 0xFF;
        break;

     case CRTC_DOSETADDR:       // R31
        crtc.update_strobe = 0;
        val = 0xFFFF;
        break;

     default:
        val = 0xFFFF;
    }

 if (modio.crtc)
    log_port_2("crtc_data_r", "reg", "val", port, reg, val);

 return val;
}

//==============================================================================
// Write CRTC register data - Port function
//
// crtc_redraw, and crtc_redraw_char functions
// -------------------------------------------
// uses: crtc.hdisp, crtc.vdisp, crtc.disp_start, crtc.scans_per_row,
//       cur_start, cur_end, cur_pos, cur_mode
//
// Setting up the display resolution
// ---------------------------------
// The CRTC_HDISP data is placed in crtc.hdisp.
// The CRTC_VDISP data is placed in crtc.vdisp.
// The CRTC_SCANLINES data is placed in crtc.scans_per_row
//
// The X resolution is determined from crtc.hdisp * 8 The Y resolution is
// determined from crtc.vdisp * crtc.scans_per_row
//
//   pass: uint16_t port
//         uint8_t data
//         struct z80_port_write *port_s
// return: void
//==============================================================================
void crtc_data_w (uint16_t port, uint8_t data, struct z80_port_write *port_s)
{
 int old_curpos;

 if (modio.crtc)
    log_port_1("crtc_data_w", "data", port, data);

 crtc_regs_data[reg] = data;

 switch (reg)
    {
     case CRTC_HTOT:            // R0
        htot = (data & 0xFF) + 1;
        crtc_calc_vsync_freq();
        break;
     case CRTC_HDISP:           // R1
        if (crtc.hdisp != (data & 0xFF)) // if the value has really changed
           {
            crtc.hdisp = data & 0xFF;
            crtc.resized = 1;
           }
        crtc_calc_vsync_freq();
        break;

     // R2 - Not implemented
     // R3 - Not implemented

     case CRTC_VTOT:            // R4
        vtot = (data & 0x7F) + 1;
        crtc_calc_vsync_freq();
        break;
     case CRTC_VTOT_ADJ:        // R5
        vtot_adj = data & 0x1F;
        crtc_calc_vsync_freq();
        break;

     case CRTC_VDISP:           // R6
        if (crtc.vdisp != (data & 0x7F)) // if the value has really changed
           {
            crtc.vdisp = data & 0x7F;
            crtc.resized = 1;
           }
        break;

     // R7 - Not implemented
     
     // R8 - Not implemented
     // Mode Control - this will normally be programmed with 01001000
     // bit 6 set=pin 34 functions as an update strobe.
     // bit 3 set=transparent memory addressing.

     case CRTC_SCANLINES:       // R9
        if (crtc.scans_per_row != ((data & 0x1F) + 1)) // if the value has
                                                       // really changed
           {
            crtc.scans_per_row = (data & 0x1F) + 1;
            crtc.resized = 1;
           }
        crtc_calc_vsync_freq();
        break;

     case CRTC_CUR_START:       // R10
        cur_start = data & 0x1F;
        cur_mode = (data >> 5) & 0x03;
        crtc_update_cursor();
        crtc_redraw_char(cur_pos, 0);
        break;
     case CRTC_CUR_END:         // R11
        cur_end = data & 0x1F;
        crtc_redraw_char(cur_pos, 0);
        break;

     case CRTC_DISP_START_H:    // R12
        crtc.disp_start &= 0xFF;
        crtc.disp_start |= (data & 0x3F) << 8;
        crtc_set_redraw();
        break;
     case CRTC_DISP_START_L:    // R13
        crtc.disp_start &= 0x3F00;
        crtc.disp_start |= data & 0xFF;
        crtc_set_redraw();
        break;

     case CRTC_CUR_POS_H:       // R14
        old_curpos = cur_pos;
        cur_pos &= 0xFF;
        cur_pos |= (data & 0x3F) << 8;
        crtc_redraw_char(old_curpos, 0);
        crtc_redraw_char(cur_pos, 0);
        break;
     case CRTC_CUR_POS_L:       // R15
        old_curpos = cur_pos;
        cur_pos &= 0x3F00;
        cur_pos |= data & 0xFF;
        crtc_redraw_char(old_curpos, 0);
        crtc_redraw_char(cur_pos, 0);
        break;

     // R16 - Is a read only register
     // R17 - Is a read only register

     case CRTC_SETADDR_H:       // R18
        mem_addr = (mem_addr & 0x00FF) | ((data & 0x3F) << 8);
        break;
     case CRTC_SETADDR_L:       // R19
        mem_addr = (mem_addr & 0x3F00) | (data & 0xFF);
        break;

     case CRTC_DOSETADDR:       // R31
        crtc.update_strobe = 0;
        if (modelx.lpen)
           keystd_handler(mem_addr);
        break;
   }
}

//==============================================================================
// redraw one screen address character position.
//
//   pass: int addr             address to be redrawn
//         int dostdio          1 if stdout should be used
// return: void
//==============================================================================
void crtc_redraw_char (int maddr, int dostdout)
{
 if ((crtc.hdisp == 0) || (! crtc.video))
    return;
 vdu_redraw_char(maddr);
}

//==============================================================================
// Set the redraw flag so that the next crtc_redraw function call is carried
// out.
//
//   pass: void
// return: void
//==============================================================================
void crtc_set_redraw (void)
{
 redraw = 1;
}

//==============================================================================
// Update the whole screen area if the global redraw flag is set, otherwise
// only those character positions that have changed.
//
//   pass: void
// return: void
//==============================================================================
void crtc_redraw (void)
{
 int i, j, x, y, l;
 int maddr;

 if (!crtc.video)
    return;                     /* redraws disabled */

 vdu_propagate_pcg_updates(crtc.disp_start, crtc.vdisp * crtc.hdisp);

 maddr = crtc.disp_start;
 l = video.yscale * crtc.scans_per_row;
 for (y = 0, i = 0; i < crtc.vdisp; i++, y += l)
    for (x = 0, j = 0; j < crtc.hdisp; j++, x += 8)
       {
        maddr &= 0x3fff;
        if (redraw || vdu_char_is_redrawn(maddr))
           {
           vdu_draw_char(screen, 
                         x, y,
                         maddr,
                         crtc.scans_per_row,
                         crtc.flashvideo,
                         (maddr == cur_pos) ? cur_blink : 0x00, cur_start, cur_end);
           vdu_char_clear_redraw(maddr);
           /* Signal to the video module that the screen needs to be redrawn */
           crtc.update = 1;
           }
        maddr++;
       }
 redraw = 0;
}

//==============================================================================
// CRTC update cursor.
//
// This function updates the state of the cursor.  It returns true if the
// cursor state has changed.
//
//   pass: void
// return: int
//==============================================================================
int crtc_update_cursor(void)
{
 // Determine the current status for the CRTC blinking cursor and refresh it
 // if this has changed.  The method used here depends on if turbo mode is
 // used. If turbo mode is used then Z80 execution speed will not be known as
 // no delays will be inserted, if not turbo then the rate must be determined
 // by the Z80 cycle count to achieve smooth results.
 switch (cur_mode)
    {
     default:
     case 0:
        cur_blink_last = cur_blink = 0xff; /* cursor always displayed */
        break;
     case 1:
        cur_blink_last = cur_blink = 0x00; /* cursor off */
        break;
     case 2:
        // blinking at 1/32 field rate
        cur_blink =
           (((emu.turbo) ?
             (time_get_ms() / cur_blink_rate_t1r32) :
             (emu.z80_cycles / cur_blink_rate_c1r32)) & 0x01) ? 0xff: 0x00;
        break;
     case 3:
        // blinking at 1/16 field rate
        cur_blink =
           (((emu.turbo) ?
             (time_get_ms() / cur_blink_rate_t1r16) :
             (emu.z80_cycles / cur_blink_rate_c1r16)) & 0x01) ? 0xff: 0x00;
        break;
    }
 if (cur_blink != cur_blink_last)
    {
     cur_blink_last = cur_blink;
     return 1;                  // changed
    }
 else
    return 0;
}


//==============================================================================
// CRTC update.
//
// Updates the CRTC periodically.
//
//   pass: void
// return: void
//==============================================================================
void crtc_update (void)
{
  if (crtc.resized)
     crtc_videochange();        // resets crtc.resized value

 if (crtc_update_cursor())
    crtc_redraw_char(cur_pos, 0);
 // Determine the current state of the alpha+ flashing video and refresh it
 // if this has changed.
 if (vdu.extendram)                    // only if extended RAM selected
    {
     if (emu.turbo)
        {
         if ((time_get_ms() / crtc.flashvalue_t) & 0x01)
            crtc.flashvideo = modelx.hwflash;
         else
            crtc.flashvideo = 0;
        }
     else
        {
         if ((emu.z80_cycles / crtc.flashvalue_c) & 0x01)
            crtc.flashvideo = modelx.hwflash;
         else
            crtc.flashvideo = 0;
        }
     if (crtc.flashvideo != flashvideo_last)
        {
         flashvideo_last = crtc.flashvideo;
         vdu_propagate_flashing_attr(crtc.disp_start, crtc.vdisp * crtc.hdisp);
        }
    }
 crtc_redraw();
}

//==============================================================================
// CRTC register dump
//
// Dump the contents of the crtc registers.
//
//   pass: void
// return: void
//==============================================================================
void crtc_regdump (void)
{
 int i;
 char s[17];

 // CRTC_CUR_POS_H
 crtc_regs_data[14] = (cur_pos >> 8) & 0x3F;

 // CRTC_CUR_POS_L
 crtc_regs_data[15] = cur_pos & 0xFF;

 // CRTC_LPEN_H
 crtc_regs_data[16] = (lpen >> 8) & 0x3F;

 // CRTC_LPEN_L
 crtc_regs_data[17] = lpen & 0xFF;

 xprintf("\n");
 xprintf("6545 CRTC Registers                Hex  Dec    Binary\n");
 xprintf("------------------------------------------------------\n");

 for (i = 0; i < 20; i++)
    xprintf("0x%02x (%02dd) %-22s  %02x %5d %10s\n", i, i, crtc_regs_names[i],
    crtc_regs_data[i], crtc_regs_data[i], i2b(crtc_regs_data[i],s));
}

//==============================================================================
// CRTC set hardware flash rate.
//
// Sets the flash rate for the alpha+ flashing attribute bit.  The flash
// rate is determined by IC60 a dual 4-bit binary counter, 4 link settings
// (W6x) and the VSYNC signal (typ 50Hz).  The settings for a V4 main board
// are as follows:
//
// Number   74LS393   Link      Rate (milliseconds)
// 0        1QA                 20
// 1        1QB                 40
// 2        1QC                 80
// 3/8      1QD       W61 A-B   160
// 4/9      2QA       W62 A-B   320
// 5/10     2QB       W63 A-B   640
// 6/11     2QC       W64 A-B   1280
// 7        2QD                 2560
//
// Four link settings (W61-W64) are provided on the main board, other values
// are possible by connecting to other pins.
//
// NOTE: Version 3 boards have 1280mS for W63 and 640mS for W64.
//
// This function should be called when the flash rate option is used and
// after the CPU clock speed is set.
//
//   pass: int n                flash rate number 0-11
// return: int                  0 if sucess, -1 if error
//==============================================================================
int crtc_set_flash_rate (int n)
{
 double t;

 if ((n < 0) || (n > 11))
    return -1;

 if (n < 8)
    crtc.flashrate = n;
 else
    crtc.flashrate = (n - 8) + 3;

 t = (1.0 / vsync_freq) * (1 << n);

 crtc.flashvalue_c = (int)(emu.cpuclock * t);
 crtc.flashvalue_t = (int)(t * 1000.0);

 return 0;
}

//==============================================================================
// CRTC clock calculations.
//
// VERTICAL BLANKING
// -----------------
// The Vertical blanking status is generated from the Z80 clock cycles that
// have elapsed or the host timer depending on the mode required.
//
// The Vertical blanking period is emulated to produce about a 15% (was 10%)
// on duty cycle. The emulated blanking frequency is ~50 frames per second
// for normal usage or can be a proportional value calculated from the CPU
// clock frequency.
//
// The vertical blanking period can not be derived from the host timer in
// the manner expected because the timer is continuous and the Z80 CPU
// emulation is achieved in frames, so a basic 50% duty is returned for this
// mode.  This is mainly intended for when running high speed emulation to
// keep key repeat speed usable.
//
// The vertical blanking is commonly used for keyboard encoding delays and for
// delays used in some games.
//
// CURSOR BLINKING
// ---------------
// blinking time (1/16 field rate) = 16 / vsync_freq
// blinking time (1/32 field rate) = 32 / vsync_freq
//
//   pass: int cpuclock         CPU clock frequency in Hz.
// return: void
//==============================================================================
void crtc_clock (int cpuclock)
{
 vblank_divval = (int)(cpuclock / vsync_freq);  // 67500 if 50Hz
 vblank_cmpval = (int)(vblank_divval * (15.0/100.0));

 // blinking at 1/32 field rate
 cur_blink_rate_t1r32 = (int)((32.0 / vsync_freq) * 1000);
 cur_blink_rate_c1r32 = (int)(cpuclock * (32.0 / vsync_freq));

 // blinking at 1/16 field rate
 cur_blink_rate_t1r16 = (int)((16.0 / vsync_freq) * 1000);
 cur_blink_rate_c1r16 = (int)(cpuclock * (16.0 / vsync_freq));

 crtc_set_flash_rate(crtc.flashrate);
}

//==============================================================================
// CRTC VSYNC calculations.
//
// Calculate the vertical sync frequency.  2MHz models use 12MHz and all
// others use a 13.5MHz crystal.
//
//   pass: void
// return: void
//==============================================================================
static void crtc_calc_vsync_freq (void)
{
 double vdu_xtal;

 if (emu.model == MOD_2MHZ)
    vdu_xtal = 12.0E+6;
 else
    vdu_xtal = 13.5E+6;

 if (htot && vtot && crtc.scans_per_row)
    {
     vsync_freq = (vdu_xtal / (htot * 8)) /
     (vtot * crtc.scans_per_row + vtot_adj);
     // adjust everything that relies on the VSYNC frequency
     crtc_clock(emu.cpuclock);
    }

 // avoid divide by 0 errors
 if (vsync_freq < 5.0)
    vsync_freq = 1.0;

#if 0
 xprintf("crtc_sync: vsync_freq=%f\n", vsync_freq);
#endif
}
