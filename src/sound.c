#ifdef MINGW
#include <windows.h>
#else
#include <sys/stat.h>
#include <signal.h>             // signal name macros, and the signal() prototype
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <SDL2/SDL.h>

#include "ubee512.h"
#include "audio.h"
#include "sound.h"
#include "z80api.h"
#include "support.h"

//==============================================================================
// constants
//==============================================================================
#define DEBUG_SPEAKER 0         /* set to 1 to debug the operation of
                                 * the microbee speaker driver */

//==============================================================================
// structures and variables
//==============================================================================

extern emu_t emu;
extern audio_t audio;

#define SPEAKER_HOLDOFF_TIME 50    /* ms */
#define SPEAKER_IDLE_TIME 1000     /* ms */
#define SPEAKER_DECAY_CONSTANT 50  /* ms */

#define SPEAKER_AMPLITUDE (AUDIO_MAXVAL / 3)

typedef struct speaker_t {
   audio_scratch_t snd_buf;
   int samples_since_write;     // counts samples since the speaker
                                // port was last written to
   uint8_t state;               // current state of the speaker
                                // output
   int idle;                    // set if the speaker hasn't changed state
                                // during the last video frame
   uint64_t change_tstates;
   int samplenumber;
   int fraction;                // position of speaker transition
                                // within a sample, used to
                                // interpolate the final value
   int last_sample;             // partial sample under construction
   int div_num, div_denom;      // numerator and denominator of the
                                // tstates -> samples conversion factor
   int idle_count;              // number of idle frames before this source
                                // stops generating samples
   int count;                   // counter
   int tau, decay;
} speaker_t;

speaker_t speaker;

int speaker_tick(audio_scratch_t *buf, const void *data,
                 uint64_t start, uint64_t cycles);
void speaker_clock(int cpuclock);


// this macro, given a time in CPU clocks, returns the number of clocks after
// the start of the current sample.  The numerator and denominator of the
// cpu clock -> sample clock conversion fraction are in S (a speaker_t
// structure)
#define SAMPLE_TIME_FRACTION(S, TSTATES)                        \
   ((TSTATES) * (S)->div_denom % (S)->div_num / (S)->div_denom)

#define SAMPLE_TIME_FRACTION_REMAINING(S, TSTATES)                      \
   (((S)->div_num - (TSTATES) * (S)->div_denom % (S)->div_num) /        \
    (S)->div_denom)

// computes the value of a partial sample given the full sample value
// and a sample fraction
#define PARTIAL_SAMPLE(S, TSTATE_FRACTION, SAMPLE)                      \
   ((SAMPLE) * (TSTATE_FRACTION) * (S)->div_denom / (S)->div_num)

// computes the number of complete samples in a number of CPU clocks
#define SAMPLE_COUNT(S, TSTATES)                \
   ((TSTATES) * (S)->div_denom / (S)->div_num)

//==============================================================================
// Speaker Initialise
//
//   pass: void
// return: int                  0 if success, -1 if error
//==============================================================================
int speaker_init (void)
{
 // register a sound source for the Microbee speaker
 audio_register(&speaker.snd_buf,
                "speaker",
                &speaker_tick, (void *)&speaker,
                &speaker_clock,
                1,
                SPEAKER_HOLDOFF_TIME
     );
 // framerate is in frames/s, so one frame is 1/framerate seconds.
 speaker.idle_count = SPEAKER_IDLE_TIME * emu.framerate / 1000;
 /* Make the audio output decay with a time constant of about
  * 50ms. Actual hardware doesn't do this; but on actual hardware
  * the sound output also never goes negative :) */
 speaker.tau = audio.frequency * SPEAKER_DECAY_CONSTANT / 1000;
 return 0;
}

//==============================================================================
// Speaker de-initialise
//
//   pass: void
// return: int                          0
//==============================================================================
int speaker_deinit (void)
{
 audio_deregister(&speaker.snd_buf);
 return 0;
}

//==============================================================================
// Set the tstates->samples conversion factor based on the current CPU
// clock and the current output sample frequency.
//
//   pass: int cpuclock             CPU clock speed in Hz
//
//   Globals used:
//         audio.mode               Set to AUDIO_PROPORTIONAL to keep the sound
//                                  pitch proportional to the CPU speed
//==============================================================================
void speaker_clock(int cpuclock)
{
 speaker_t *s = &speaker;
 uint64_t cycles_now = z80api_get_tstates();

 if (audio.mode != AUDIO_PROPORTIONAL)
    cpuclock = 3375000;

 speaker.div_denom = audio.frequency;
 speaker.div_num = cpuclock;

 {
  int a = speaker.div_denom, b = speaker.div_num, t;

  /* A should be > B */
  if (a < b)
     {
      t = a;
      a = b;
      b = t;
     }
  while (b != 0)                /* compute GCD */
     {
      t = b;
      b = a % b;
      a = t;
     }
  speaker.div_num /= a;
  speaker.div_denom /= a;
 }

 // The current sample number and partial sample counts also
 // need to be updated here
 s->samplenumber = SAMPLE_COUNT(s, cycles_now);
 s->fraction = SAMPLE_TIME_FRACTION(s, cycles_now);
}

//==============================================================================
// Speaker sample
//
//   pass: uint8_t data                 non-zero if speaker bit set
//                                      zero if speaker bit clear
// return: int
//==============================================================================
inline int speaker_sample(uint8_t data)
{
 // The maximum amplitude is set to be 1/3 of the absolute maximum, so that
 // the speaker is as loud as the BeeThoven output.
 return (data) ? +SPEAKER_AMPLITUDE : -SPEAKER_AMPLITUDE;
}

//==============================================================================
// Speaker sample fixup.  Integer rounding errors can accrue to the point
// where an accumulated sample doesn't quite add up to SPEAKER_AMPLITUDE
// which leads to an annoying buzz in the output.
//
//   pass: int                          sample
// return: int
//==============================================================================
inline int speaker_fixup_sample(int sample)
{
 if (sample >= SPEAKER_AMPLITUDE - 2)
    sample = SPEAKER_AMPLITUDE;
 else if (sample <= -(SPEAKER_AMPLITUDE - 2))
    sample = -SPEAKER_AMPLITUDE;
 return sample;
}

//==============================================================================
// Speaker reset
//
//   pass: void
// return: int                          sound init result
//==============================================================================
int speaker_reset (void)
{
 speaker_t *s = &speaker;
 audio_scratch_t *sb = &s->snd_buf;

 s->state = 0;
 s->change_tstates = z80api_get_tstates();
 s->decay = 0;
 s->fraction = 0;
 s->last_sample = 0;

 // If there is an audio buffer under construction - dump it, the
 // next call to speaker_fill will get a fresh one.
 if (audio_has_work_buffer(sb))
    audio_put_work_buffer(sb);

 return 0;
}

void speaker_fill(speaker_t *s, int sample, int count)
{
 audio_scratch_t *sb = &s->snd_buf;
#if DEBUG_SPEAKER
    xprintf("speaker_fill: writing %d of %d\n", count, sample);
#endif /* DEBUG_SPEAKER */
 // fill the buffer with the speaker value.
 while (count)
    {
     int n;
     /* flush the current work buffer if it is full */
     if (audio_space_remaining(sb) == 0)
         audio_put_work_buffer(sb);
     /* get a fresh sound buffer if necessary */
     if (!audio_has_work_buffer(sb))
        audio_get_work_buffer(sb);
     /* work out how many samples will fit in the current buffer */
     n = audio_space_remaining(sb);
     if (n > count)
        n = count;
     count -= n;
     while (n--)
        {
         s->decay -= ((sample * (1 << 16)) + s->decay) / s->tau;
         // delay applying the decay value until after it becomes
         // significant...  FIXME: necessary?
         if (s->decay > 2 * (1 << 16) || s->decay < -2 * (1 << 16))
            audio_put_sample(sb, audio_limit(sample + (s->decay / (1 << 16))));
         else
            audio_put_sample(sb, audio_limit(sample));
        }
    }
}

//==============================================================================
// Speaker update
//
//   pass: speaker_t *
//         uint_8
// return: void
//
// speaker_update generates audio samples since the last speaker bit change.
//==============================================================================

void speaker_update(speaker_t *s, uint8_t data)
{
 uint64_t cycles_now;
 int samplenumber_now;
 int fraction_now;
 int fraction_diff;
 unsigned int n;
 int sample = speaker_sample(s->state);
 int fractional_sample;

 cycles_now = z80api_get_tstates();

#if DEBUG_SPEAKER
 xprintf("speaker_update: cycles_now %llu, cycles_then %llu\n",
         cycles_now, s->change_tstates);
#endif /* DEBUG_SPEAKER */

 /* if there is no current buffer, obtain one.  In this case the
  * audio source has been idle for some time, so we assume the
  * last sample to be zero and the last state change to be now */
 if (!audio_has_work_buffer(&s->snd_buf))
    {
     audio_get_work_buffer(&s->snd_buf);
     s->change_tstates = cycles_now;
     s->last_sample = 0;
     s->samplenumber = SAMPLE_COUNT(s, s->change_tstates);
     s->fraction = SAMPLE_TIME_FRACTION(s, s->change_tstates);
#if DEBUG_SPEAKER
     xprintf("speaker_update: "
             "initial partial sample: %d * %d/%d of %d = %d\n",
             s->fraction, s->div_denom, s->div_num,
             speaker_sample(s->state), s->last_sample);
#endif /* DEBUG_SPEAKER */
    }

 samplenumber_now = SAMPLE_COUNT(s, cycles_now);
 fraction_now = SAMPLE_TIME_FRACTION(s, cycles_now);

 if (samplenumber_now == s->samplenumber)
    {
     /* Only the partial sample needs to be updated, we don't need
      * to emit it yet */
#if DEBUG_SPEAKER
     xprintf("speaker_update: "
             "updated partial sample %d ", s->last_sample);
#endif /* DEBUG_SPEAKER */
     fraction_diff = fraction_now - s->fraction;
     fractional_sample = PARTIAL_SAMPLE(s, fraction_diff, sample);
     s->last_sample += fractional_sample;
#if DEBUG_SPEAKER
     xprintf("with %d * %d/%d of %d = %d ",
             fraction_diff, s->div_denom, s->div_num,
             sample, fractional_sample);
     xprintf("result %d\n", s->last_sample);
#endif /* DEBUG_SPEAKER */
     /* the sample number remains unchanged */
    }
 else
    {
     // need to finish off the partial sample from the last call
     // to speaker_update();
     fraction_diff = SAMPLE_TIME_FRACTION_REMAINING(s, s->change_tstates);
     fractional_sample = PARTIAL_SAMPLE(s, fraction_diff, sample);
     s->last_sample += fractional_sample;
     s->last_sample = speaker_fixup_sample(s->last_sample);
#if DEBUG_SPEAKER
     xprintf("speaker_update: "
             "updated partial sample: %d * %d/%d of %d = %d\n",
             fraction_diff, s->div_denom, s->div_num,
             sample, fractional_sample);
#endif /* DEBUG_SPEAKER */
#if DEBUG_SPEAKER
     xprintf("speaker_update: value %d\n", s->last_sample);
#endif /* DEBUG_SPEAKER */
     assert(s->last_sample >= -(AUDIO_MAXVAL + 1) &&
            s->last_sample <= AUDIO_MAXVAL);
     speaker_fill(s, s->last_sample, 1);
     s->samples_since_write++;
     // write out complete samples
     n = samplenumber_now - s->samplenumber - 1;
     speaker_fill(s, sample, n);
     s->samples_since_write += n;
     // and record the final partial sample.
     s->last_sample = PARTIAL_SAMPLE(s, fraction_now, sample);
#if DEBUG_SPEAKER
     xprintf("speaker_update: "
             "created partial sample: %d * %d/%d of %d = %d\n",
             fraction_now, s->div_denom, s->div_num,
             sample, s->last_sample);
#endif /* DEBUG_SPEAKER */
    }
 s->fraction = fraction_now;
 s->samplenumber = samplenumber_now;
 s->state = data;
 s->change_tstates = cycles_now;
}

//==============================================================================
// Speaker write
//
//   pass: uint8_t data                 non-zero if speaker bit set
//                                      zero if speaker bit clear
// return: void
//==============================================================================
void speaker_w (uint8_t data)
{
 speaker_t *s = &speaker;

 // only do something if the speaker state changes.
 if (data == s->state)
    return;

#if DEBUG_SPEAKER
 xprintf("speaker_w: writing %02x\n", data);
#endif /* DEBUG_SPEAKER */
 // if this is the first update since the speaker source was marked idle
 // and stopped generating samples, just update the last update time, don't
 // actually write anything into the buffer yet.
 if (s->idle && s->count == 0)
    {
     s->last_sample = 0;
     s->state = data;
     s->change_tstates = z80api_get_tstates();
    }
 else
    speaker_update(s, data);
 s->idle = 0;
 s->count = s->idle_count;
 s->samples_since_write = 0;
}

//==============================================================================
// Speaker tick function, called at the end of every block of Z80
// instructions
//==============================================================================
int speaker_tick(audio_scratch_t *buf, const void *data,
                 uint64_t start, uint64_t cycles)
{
 speaker_t *s = (speaker_t *)data;

 if (!audio_has_work_buffer(&s->snd_buf))
    goto idle;

 if (s->change_tstates == start)
    {
     if (s->idle)
        {
         if (s->count > 0)
            s->count--;
         else
            goto idle;
        }
     else
        {
         s->idle = 1;
         s->count = s->idle_count;
        }
    }

#if DEBUG_SPEAKER
// insert a very marked click into the audio stream
// speaker_fill(s, 128, 1);
// speaker_fill(s, -128, 1);
 xprintf("speaker_tick:\n");
#endif /* DEBUG_SPEAKER */
 speaker_update(s, s->state);

 if (s->idle && s->count == 0)
 {
  audio_put_work_buffer(&s->snd_buf); // flush current buffer.
  s->decay = 0;                       // reset decay constant
 }
 return 1;

idle:
 s->change_tstates = start + cycles;
 return 0;
}
