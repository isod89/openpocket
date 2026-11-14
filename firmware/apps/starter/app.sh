# STARTER (starter.c): what firmware/build.sh needs to build it on the base firmware (docs/BASE-FIRMWARE.md).
APP_MAIN=starter.c         # the app's C file
APP_ENGINE=engine.c        # the engine's C file (compiled -O2, as engine/px_dsp.c is)
APP_DEFS=                  # compiler options for both
APP_RAM_DSP="se_run"       # the engine file's functions that run from RAM
APP_RAM_FW="app_sound"     # starter.c's functions that run from RAM, beside the base's audio path
