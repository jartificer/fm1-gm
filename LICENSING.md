# Licensing

FM1-GM is free software under the GNU General Public License, version 3 only (`LICENSE`).

It is built on, and includes code from:

- [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita (@kurogedelic), Hügelton Instruments, GPL-3.0: the FM-1 hardware layer, the update loader, the package tools, the installer and the rescue tool.
- [X0X](https://github.com/charlesvestal/fm1-x0x) and [FoMni](https://github.com/charlesvestal/fm1-fomni), GPL-3.0: the platform interface, the panel map, the flash store and the host test harness.

The GM engine, the sequencer, the song storage and the UI are new to this project. The General MIDI program and drum names are the standard GM1 names.

The instrument and drum samples in `assets/soundfont/fm1gm.sf2`, the sustain fits made from them (`assets/soundfont/fm1gm_sustain.json`) and the tables the build makes from both are a reduced version of the Fluid R3 GM SoundFont, Copyright (c) 2000-2002, 2008 Frank Wen and 2008 Toby Smithe, MIT License (`assets/soundfont/LICENSE`).

Fonts in `assets/fonts/`:

- Barlow Semi Condensed, SIL Open Font License 1.1 (`Barlow-OFL.txt`).
- Terminus, SIL Open Font License 1.1 (`Terminus-LICENSE.txt`).

The JieLi toolchain and the AC79 SDK are not part of this repository; the build fetches them.

M-VAVE and FM-1 are trademarks of their owner. This project is not affiliated with M-VAVE.
