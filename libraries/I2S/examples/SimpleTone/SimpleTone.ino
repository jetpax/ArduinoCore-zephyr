/*
 * I2S SimpleTone for PiZZa
 *
 * Plays a 440 Hz square wave on the PCM / I2S interface of the Pi
 * 40-pin header:
 *
 *   BCK   GPIO 18 (header pin 12)
 *   LRCK  GPIO 19 (header pin 35)
 *   DIN   GPIO 20 (header pin 38)
 *   DOUT  GPIO 21 (header pin 40)
 *
 * Wire DOUT + BCK + LRCK to any 16-bit I2S DAC or amplifier
 * (e.g. MAX98357A, UDA1334A, WM8960).
 */

#include <I2S.h>

const int frequency = 440;
const int amplitude = 5000;
const long sampleRate = 44100;

const int halfWavelength = sampleRate / frequency / 2;

short sample = amplitude;
int count = 0;

void setup() {
  Serial.begin(9600);

  if (!I2S.begin(I2S_PHILIPS_MODE, sampleRate, 16)) {
    Serial.println("Failed to initialize I2S!");
    while (1);
  }
}

void loop() {
  if (count % halfWavelength == 0) {
    sample = -sample;
  }

  I2S.write(sample);  // left channel
  I2S.write(sample);  // right channel

  count++;
}
