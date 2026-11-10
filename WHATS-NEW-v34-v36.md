# What's new in OpenPocket since v33 (v34, v35, v36)

These three updates focus on one thing: sound quality you can trust, rather than a long list of models.

## v34: amps, drives and the 4x12 fitted to real gear
- The 29 amps and 27 drives that were voiced by ear are replaced by **8 amps and 4 drives whose settings were fitted
  by computer to Neural Amp Modeler captures of the real units**, played with real DI guitar:
  - amps: Twin Reverb, AC15 Top Boost, JCM900, JCM2000, Mark IV, Dirty Shirley, Pro Mod, 5150;
  - drives: TS9, Klon, MXR M77, Boss HM-2.
- **Measured accuracy:** the difference in tone from the capture dropped from 10 to 19 dB to 0.3 to 2.3 dB on the
  amps, on 20 seconds of guitar the fitting never heard. Below about 1.5 dB the two are hard to tell apart.
- **The 4x12 V30 cabinet** was fitted to measured responses; its error went from 4.1 dB to 1.5 dB.
- **Same engine, same CPU cost:** only the numbers changed.
- **Your saved presets:** they move to the nearest new model automatically. The web editor converts old preset files
  the same way.

## v35: new reverbs, 2-second delay, much more free memory
- **New reverbs.** The old Freeverb is replaced by:
  - an 8-line feedback delay network with early reflections and slowly moving lines, for the rooms, halls, spring,
    shimmer, gated and ambient types;
  - Dattorro's classic plate for PLATE.
- **What it changes:**
  - DECAY now gives the real decay time (HALL at noon: 2.6 s measured for 2.6 s set);
  - TONE sets how fast the highs die away;
  - long tails ring less (closer to a real room).
- **Delay up to 2 seconds,** with smoother repeats when the time glides or the tape wobbles. The chorus, flanger,
  vibrato and rotary got the same improvement.
- **Delay and reverb now use 16-bit memory.** This freed 89 KB of RAM, which leaves room for future features such
  as BLE and MIDI.
- **Saved presets keep the same delay time.**

## v36: pro-level levels, curated presets, a new screen
- **Everything at the same loudness.** Levels are now measured as loudness on real guitar (the broadcast standard,
  ITU BS.1770). The old factory presets spanned 43 dB, from far too quiet to very loud. Now:
  - every amp sounds equally loud at GAIN noon and moves only ±3 dB over the whole knob;
  - every effect, with its knobs at noon, keeps the volume the same when you switch it on;
  - all 24 factory presets sit within 0.2 dB of each other.
- **New PRESET LVL setting** (GT page, 3rd line, −15 to +15 dB) to balance your own sounds.
- **24 curated factory presets** built around the fitted amps:
  - clean: Twin Clean, Chorus Clean, AC15 Chime, Funk Wah, Surf Spring, Ambient Swell;
  - crunch: TS Blues, Blues Breakup, Klon Crunch, Brit Crunch, 70s Rock, Boutique;
  - lead: Brit Lead, Smooth Lead, Hair Metal, Space Lead;
  - high gain: Modern Rock, 5150 Rhythm, Tight Metal, Swede Death, Doom;
  - effects: Vibe Rock, Organ, Acoustic.

  Slots 25 to 52 are USER slots: a clean starting point for your own sounds.
- **A soft output limiter** replaces the hard clipping at the output.
- **Smaller sound fixes:**
  - the SLOW SWELL no longer lets the pick attack through;
  - gentle noise gates were added on the cleans that hissed.
- **A new screen,** inspired by the Helix, HX Stomp and Boss GX:
  - a PLAY page you can read from a distance: big preset number and name, the amp and cab, the chain in colour,
    the selected block;
  - an EDIT page with the block in its colour and big values with bars;
  - a restyled tuner and save page.
- **Battery gauge** at the top centre of the PLAY page (percentage, or "USB" when plugged in). The tuner page also
  shows the battery voltage.
- **The output is muted while the tuner is open.**
- **Fixes:**
  - the volume showed "VOL 10" at 100;
  - the delay time is now shown in milliseconds.
- **Offline installer:** `INSTALL-OFFLINE.bat` installs the firmware with no internet connection.

Your saved presets are kept through all three updates and converted automatically. Files exported from the web
editor with older versions still import.
