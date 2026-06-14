#ifndef MORSE_H
#define MORSE_H

#include "Common.h"

//
// Morse (CW) decoder.
//
// The decoder is fed by a pluggable signal source that returns a
// normalized envelope sample. Two sources are supported:
//
//  * MORSE_RSSI  - reconstructs the on/off envelope from the SI4732
//                  RSSI/SNR readings (works on the stock V3 hardware
//                  without any modification).
//  * MORSE_AUDIO - samples the demodulated audio fed back to an ADC
//                  pin (GPIO11/ADC2). On the V3 this requires a small
//                  hardware mod (wire from the audio output to IO11
//                  with an RC low pass filter); it is routed from the
//                  factory only on the V4. The audio source is always
//                  compiled in and selectable on screen; the menu warns
//                  that it needs the mod. Without it, the pin is
//                  unconnected and decoding is meaningless (user's risk).
//

// Morse decoder modes (also the index into the mode selector)
#define MORSE_OFF    0
#define MORSE_RSSI   1
#define MORSE_AUDIO  2

// Audio input ADC pin used by the optional audio source / hardware mod
#ifndef MORSE_AUDIO_PIN
#define MORSE_AUDIO_PIN 11
#endif

extern uint8_t morseModeIdx;

// Lifecycle
void morseSetup();
void morseReset();

// Periodic processing, returns TRUE when the decoded text changed
bool morseTickTime();

// Queries
bool morseIsEnabled();
const char *morseGetText();
int getTotalMorseModes();
const char *morseModeName(uint8_t idx);

#endif // MORSE_H
