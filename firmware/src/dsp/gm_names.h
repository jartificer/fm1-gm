/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-GM: the names of the 128 General MIDI programs, their 16 families and the GM drum kit's
 * notes 35..81, for the screens. Included by gm.c only. The sounds are in the wavetable
 * (gm_wave.h). */
#pragma once

static const char *const GM_NAME[128] = {
    "Acoustic Grand", "Bright Piano", "Electric Grand", "Honky-tonk", "E.Piano 1", "E.Piano 2", "Harpsichord",
    "Clavi",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Perc. Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica",
    "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdrive Gtr",
    "Distortion Gtr", "Gtr Harmonics",
    "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1",
    "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato", "Harp", "Timpani",
    "Strings 1", "Strings 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice",
    "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1",
    "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead",
    "Bass + Lead",
    "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad",
    "Sweep Pad",
    "Rain", "Soundtrack", "Crystal", "Atmosphere", "Brightness", "Goblins", "Echoes", "Sci-fi",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum",
    "Reverse Cymbal",
    "Gtr Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone", "Helicopter", "Applause",
    "Gunshot",
};

static const char *const GM_FAMILY[16] = {
    "Piano", "Mallets", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass",   /* short: they */
    "Reed", "Pipe", "Lead", "Pad", "Synth FX", "Ethnic", "Perc", "SFX",              /* fit a knob */
};

static const char *const GM_DRUM_NAME[GM_DRUM_HI - GM_DRUM_LO + 1] = {
    "Kick 2", "Kick 1", "Side Stick", "Snare 1", "Hand Clap", "Snare 2", "Low Floor Tom", "Closed Hat",
    "High Floor Tom", "Pedal Hat", "Low Tom", "Open Hat", "Low-Mid Tom", "Hi-Mid Tom", "Crash 1", "High Tom",
    "Ride 1", "China", "Ride Bell", "Tambourine", "Splash", "Cowbell", "Crash 2", "Vibraslap", "Ride 2",
    "Hi Bongo", "Low Bongo", "Mute Conga", "Open Conga", "Low Conga", "Hi Timbale", "Low Timbale",
    "Hi Agogo", "Low Agogo", "Cabasa", "Maracas", "Short Whistle", "Long Whistle", "Short Guiro",
    "Long Guiro", "Claves", "Hi Woodblock", "Low Woodblock", "Mute Cuica", "Open Cuica", "Mute Triangle",
    "Open Triangle",
};
