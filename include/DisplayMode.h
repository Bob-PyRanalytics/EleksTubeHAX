#ifndef DISPLAY_MODE_H
#define DISPLAY_MODE_H

#include "GLOBAL_DEFINES.h"
#include "TFTs.h"

#define DISPLAY_PAYLOAD_MAX_LEN 256 // Longest display message kept, including the terminating '\0'

/*
 * What the displays show: the clock, images chosen over MQTT, or a countdown.
 * The payloads and rules are in docs/mqtt-display-protocol.md.
 */
class DisplayMode
{
public:
  enum modes
  {
    clock,
    tiles,
    countdown
  };

  // Apply a JSON payload from the display topic. Returns true if every display needs redrawing.
  bool apply(const char *payload);
  // Apply a message that was waiting for the time, follow the countdown, and go back to the clock once the
  // current message has expired. Returns true if every display needs redrawing.
  bool loop();

  modes getMode() { return mode; }
  // Image name for a display position, counted from the left (0 to NUM_DIGITS - 1). Empty means black.
  const char *getTile(uint8_t position) { return tile_names[position]; }

private:
  modes mode = clock;
  int64_t expires = 0; // Unix time (UTC); 0 means the message doesn't expire
  char tile_names[NUM_DIGITS][TILE_NAME_MAX_LEN + 1] = {};

  // Countdown
  int64_t until = 0;   // Unix time (UTC) the countdown reaches zero
  uint8_t face = 1;    // Clock-face set whose digits are drawn
  char blank[TILE_NAME_MAX_LEN + 1] = {}; // Drawn instead of leading zero hours; empty: draw the zeros

  // A message that carries a time but arrived before the clock had one, applied once it has.
  char pending[DISPLAY_PAYLOAD_MAX_LEN] = {};

  void updateCountdown();
  static bool isValidTileName(const char *name);
};

extern DisplayMode display_mode;

#endif // DISPLAY_MODE_H
