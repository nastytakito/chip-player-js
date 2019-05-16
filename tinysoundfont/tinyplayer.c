//
// TinyPlayer
// a tiny wrapper for TinySoundFont and TinyMidiLoader.
// Created by Matt Montag on 9/4/18.
//
#define TML_NO_STDIO
#define TML_IMPLEMENTATION

#include "tml.h"

#define TSF_IMPLEMENTATION

#include "tsf.h"

#include "../fluidlite/include/fluidlite.h"

#include <emscripten.h>

#ifdef __cplusplus
extern "C" {
#endif

//TODO: Remove debug logging (EM_ASM_)
tsf *g_TSF;                  // instance of TinySoundFont
fluid_synth_t *g_FluidSynth; // instance of FluidSynth
tml_message *g_MidiEvt;      // next message to be played
tml_message *g_FirstEvt;     // first event in current file
double g_MidiTimeMs;         // current playback time
double g_Speed = 1.0;
int g_SampleRate;
unsigned int g_DurationMs = 0;
char g_ChannelsInUse[16];
char g_ChannelsMuted[16];
int g_ChannelProgramNums[16];

// TODO: TinySoundFont not implemented
extern void tp_init(int sampleRate) {
  fluid_settings_t *settings = new_fluid_settings();
  fluid_settings_setstr(settings, "synth.reverb.active", "yes");
  fluid_settings_setstr(settings, "synth.chorus.active", "no");
  fluid_settings_setint(settings, "synth.threadsafe-api", 0);
  fluid_settings_setnum(settings, "synth.gain", 0.5);
  fluid_settings_setnum(settings, "synth.sample-rate", sampleRate);
  g_FluidSynth = new_fluid_synth(settings);
  fluid_synth_set_interp_method(g_FluidSynth, -1, FLUID_INTERP_LINEAR);
  g_SampleRate = sampleRate;
}

// Returns the number of bytes written. Value of 0 means the song has ended.
extern int tp_write_audio(float *buffer, int bufferSize) {
  int bytesWritten = 0;
  int batchSize = 128; // Timing of MIDI events will be quantized by the sample batch size.

  double msPerBatch = g_Speed * 1000.0 * (batchSize / (float) g_SampleRate) / 2;
  for (int samplesRemaining = bufferSize * 2; samplesRemaining > 0; samplesRemaining -= batchSize) {
    //We progress the MIDI playback and then process `batchSize` samples at once
    if (batchSize > samplesRemaining) batchSize = samplesRemaining;

    //Loop through all MIDI messages which need to be played up until the current playback time
    for (g_MidiTimeMs += msPerBatch;
         g_MidiEvt && g_MidiTimeMs >= g_MidiEvt->time;
         g_MidiEvt = g_MidiEvt->next) {
      switch (g_MidiEvt->type) {
        case TML_NOTE_ON:
          if (g_ChannelsMuted[g_MidiEvt->channel]) break;
          if (g_FluidSynth) {
            fluid_synth_noteon(g_FluidSynth, g_MidiEvt->channel, g_MidiEvt->key, g_MidiEvt->velocity);
          } else {
            tsf_channel_note_on(g_TSF, g_MidiEvt->channel, g_MidiEvt->key, g_MidiEvt->velocity / 127.0f);
          }
          break;
        case TML_NOTE_OFF:
          if (g_ChannelsMuted[g_MidiEvt->channel]) break;
          if (g_FluidSynth) {
            fluid_synth_noteoff(g_FluidSynth, g_MidiEvt->channel, g_MidiEvt->key);
          } else {
            tsf_channel_note_off(g_TSF, g_MidiEvt->channel, g_MidiEvt->key);
          }
          break;
        case TML_PROGRAM_CHANGE: // program change (special handling for 10th MIDI channel with drums)
          if (g_FluidSynth) {
            fluid_synth_program_change(g_FluidSynth, g_MidiEvt->channel, g_MidiEvt->program);
          } else {
            tsf_channel_set_presetnumber(g_TSF, g_MidiEvt->channel, g_MidiEvt->program, g_MidiEvt->channel == 9);
          }
          break;
        case TML_PITCH_BEND:
          if (g_FluidSynth) {
            fluid_synth_pitch_bend(g_FluidSynth, g_MidiEvt->channel, g_MidiEvt->pitch_bend);
          } else {
            tsf_channel_set_pitchwheel(g_TSF, g_MidiEvt->channel, g_MidiEvt->pitch_bend);
          }
          break;
        case TML_CONTROL_CHANGE:
          if (g_FluidSynth) {
            fluid_synth_cc(g_FluidSynth, g_MidiEvt->channel, g_MidiEvt->control, g_MidiEvt->control_value);
          } else {
            tsf_channel_midi_control(g_TSF, g_MidiEvt->channel, g_MidiEvt->control, g_MidiEvt->control_value);
          }
          break;
        default:
          break;
      }

    }
    // Render the block of audio samples in float format
    if (g_FluidSynth) {
      fluid_synth_write_float(g_FluidSynth, batchSize / 2, buffer, 0, 2, buffer, 1, 2); // interleaved stereo
    } else {
      tsf_render_float(g_TSF, buffer, batchSize / 2, 0);
    }

    buffer += batchSize;
    bytesWritten += batchSize;
  }

  if (g_MidiEvt == NULL) {
    // Last MIDI event has been processed.
    // Continue synthesis until silence is detected.
    // This allows voices with a long release tail to complete.
    // Crude method: when entire buffer is below threshold, consider it silence.
    int synthStillActive = 0;
    float threshold = 0.05;
    for (int i = 0; i < bufferSize; i++) {
      if (buffer[i * 2] > threshold) { // Check left channel only
        synthStillActive = 1;          // Exit early
        break;
      }
    }
    if (synthStillActive == 0) return 0;
  }

  return bytesWritten;
}

extern unsigned int tp_get_duration_ms() {
  if (g_DurationMs == 0 && g_FirstEvt) {
    tml_get_info(g_FirstEvt, NULL, NULL, NULL, NULL, &g_DurationMs);
  }
  return g_DurationMs;
}

extern void tp_seek(int ms) {
  // It's only possible to seek forward due to the statefulness of the synth.
  // If we need to seek backward, reset to the first event and seek forward from there.
  if (ms < g_MidiTimeMs) {
    g_MidiEvt = g_FirstEvt;
  }

  if (g_FluidSynth) {
    for (int i = 0; i < 16; i++)
      fluid_synth_all_notes_off(g_FluidSynth, i);
  } else {
    tsf_note_off_all(g_TSF);
  }

  while (g_MidiEvt && g_MidiEvt->time < ms) {
    switch (g_MidiEvt->type) {
      // Ignore note on/note off events during seek
      case TML_PROGRAM_CHANGE: // program change (special handling for 10th MIDI channel with drums)
        if (g_FluidSynth) {
          fluid_synth_program_change(g_FluidSynth, g_MidiEvt->channel, g_MidiEvt->program);
        } else {
          tsf_channel_set_presetnumber(g_TSF, g_MidiEvt->channel, g_MidiEvt->program, g_MidiEvt->channel == 9);
        }
        break;
      case TML_PITCH_BEND:
        if (g_FluidSynth) {
          fluid_synth_pitch_bend(g_FluidSynth, g_MidiEvt->channel, g_MidiEvt->pitch_bend);
        } else {
          tsf_channel_set_pitchwheel(g_TSF, g_MidiEvt->channel, g_MidiEvt->pitch_bend);
        }
        break;
      case TML_CONTROL_CHANGE:
        if (g_FluidSynth) {
          if (g_MidiEvt->control == 91) break; // ignore reverb CC from MIDI files
          fluid_synth_cc(g_FluidSynth, g_MidiEvt->channel, g_MidiEvt->control, g_MidiEvt->control_value);
        } else {
          tsf_channel_midi_control(g_TSF, g_MidiEvt->channel, g_MidiEvt->control, g_MidiEvt->control_value);
        }
        break;
      default:
        break;
    }
    g_MidiEvt = g_MidiEvt->next;
  }

  g_MidiTimeMs = ms;
}

extern double tp_get_position_ms() {
  return g_MidiTimeMs;
}

extern void tp_set_speed(float speed) {
  g_Speed = fmax(fmin(speed, 10.0), 0.1);
}

extern void tp_stop() {
  if (g_FluidSynth) {
    for (int i = 0; i < 16; i++)
      fluid_synth_all_notes_off(g_FluidSynth, i);
  } else {
    tsf_reset(g_TSF);
  }
  g_MidiEvt = NULL;
}

extern void tp_restart() {
  if (g_FluidSynth) {
    for (int i = 0; i < 16; i++)
      fluid_synth_all_notes_off(g_FluidSynth, i);
  } else {
    tsf_reset(g_TSF);
  }
  g_MidiEvt = g_FirstEvt;
}

// TODO: TinySoundFont not currently used
extern void tp_open_tsf(tsf *t, const void *data, int length, int sampleRate) {
  g_MidiTimeMs = 0;
  g_DurationMs = 0;
  g_TSF = t;
  g_FluidSynth = NULL;
  g_SampleRate = sampleRate;

  tsf_set_output(g_TSF, TSF_STEREO_INTERLEAVED, sampleRate, 0.0);
  // Channel 10 uses percussion sound bank
  tsf_channel_set_bank_preset(g_TSF, 9, 128, 0);
  g_MidiEvt = tml_load_memory(data, length);
  g_FirstEvt = g_MidiEvt;

  // Skip to first note to eliminate silence
  unsigned int firstNoteTimeMs;
  tml_get_info(g_FirstEvt, NULL, NULL, NULL, &firstNoteTimeMs, NULL);
  g_MidiTimeMs = (double)firstNoteTimeMs;

  EM_ASM_({ console.log('Tiny MIDI Player loaded %d bytes.', $0); }, length);
  EM_ASM_({ console.log('First note appears at %d ms.', $0); }, g_MidiTimeMs);
}

extern void tp_open(const void *data, int length) {
  g_MidiTimeMs = 0;
  g_DurationMs = 0;
  fluid_synth_system_reset(g_FluidSynth);
  fluid_synth_program_reset(g_FluidSynth);
  g_MidiEvt = tml_load_memory(data, length);
  g_FirstEvt = g_MidiEvt;
  memset(g_ChannelsInUse, 0, sizeof g_ChannelsInUse);
  memset(g_ChannelsMuted, 0, sizeof g_ChannelsMuted);
  tml_get_channels_in_use_and_initial_programs(g_MidiEvt, g_ChannelsInUse, g_ChannelProgramNums);

  // Skip to first note to eliminate silence
  unsigned int firstNoteTimeMs;
  tml_get_info(g_FirstEvt, NULL, NULL, NULL, &firstNoteTimeMs, NULL);
  g_MidiTimeMs = (double)firstNoteTimeMs;

  EM_ASM_({ console.log('Tiny MIDI Player loaded %d bytes.', $0); }, length);
  EM_ASM_({ console.log('First note appears at %d ms.', $0); }, g_MidiTimeMs);
}

extern void tp_unload_soundfont() {
  if (fluid_synth_sfcount(g_FluidSynth) > 0) {
    fluid_sfont_t *sfont = fluid_synth_get_sfont(g_FluidSynth, 0);
    fluid_synth_remove_sfont(g_FluidSynth, sfont);
    // Causes a crash related to pthreads in Emscripten.
    // fluid_synth_sfunload(g_FluidSynth, (unsigned)g_SoundFontID, 1);
  }
}

// TODO: TinySoundFont not implemented
extern int tp_load_soundfont(const char *filename) {
  tp_unload_soundfont();
  return fluid_synth_sfload(g_FluidSynth, filename, 1);
}

extern int tp_add_soundfont(const char *filename) {
  return fluid_synth_sfload(g_FluidSynth, filename, 1);
}

extern void tp_set_reverb(double level) {
  // Completely disable reverb at very low levels to improve performance
  fluid_synth_set_reverb_on(g_FluidSynth, level > 0.01);
  fluid_synth_set_reverb(
          g_FluidSynth,
          0.2 + level * 0.8, // roomsize (default: 0.2) 0.2 to 1.0
          0.0 + level * 0.4, // damp     (default: 0.0) 0.0 to 0.4
          1.0,               // width    (default: 0.5) 1.0
          1.0);              // level    (default: 0.9) 1.0
  // Override MIDI channel reverb levels
  for (int i = 0; i < 16; i++)
    fluid_synth_cc(g_FluidSynth, i, 91, 64);
}

extern char tp_get_channel_in_use(int chan) {
  return g_ChannelsInUse[chan] != 0;
}

extern int tp_get_channel_program(int chan) {
  return g_ChannelProgramNums[chan];
}

extern void tp_set_channel_mute(int chan, char isMuted) {
  g_ChannelsMuted[chan] = isMuted;
  if (isMuted) {
    fluid_synth_all_notes_off(g_FluidSynth, chan);
  }
}

#ifdef __cplusplus
}
#endif