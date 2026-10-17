#ifndef DISPLAY_MODE_H
#define DISPLAY_MODE_H

#include "GLOBAL_DEFINES.h"
#include "TFTs.h"

/*
 * What the displays show: the clock, or images chosen over MQTT.
 * The payloads and rules are in docs/mqtt-display-protocol.md.
 */
class DisplayMode
{
public:
  enum modes
  {
    clock,
    tiles
  };

  // Apply a JSON payload from the display topic. Returns true if every display needs redrawing.
  bool apply(const char *payload);
  // Go back to the clock once the current message has expired. Returns true if every display needs redrawing.
  bool loop();

  modes getMode() { return mode; }
  // Image name for a display position, counted from the left (0 to NUM_DIGITS - 1). Empty means black.
  const char *getTile(uint8_t position) { return tile_names[position]; }

private:
  modes mode = clock;
  int64_t expires = 0; // Unix time (UTC); 0 means the message doesn't expire
  char tile_names[NUM_DIGITS][TILE_NAME_MAX_LEN + 1] = {};

  static bool isValidTileName(const char *name);
};

extern DisplayMode display_mode;

#endif // DISPLAY_MODE_H
