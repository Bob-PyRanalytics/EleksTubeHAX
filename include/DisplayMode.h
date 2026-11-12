#ifndef DISPLAY_MODE_H
#define DISPLAY_MODE_H

#include <ArduinoJson.h>
#include "GLOBAL_DEFINES.h"
#include "TFTs.h"
#include "Backlights.h"

#define DISPLAY_PAYLOAD_MAX_LEN 256 // Longest display or countdown message kept, including the terminating '\0'

/*
 * What the displays show: the clock, images chosen over MQTT, or a countdown.
 * The payloads and rules are in docs/mqtt-display-protocol.md.
 *
 * Two retained topics feed it: "display" (clock or tiles, rotated by the publisher) and
 * "countdown". A running countdown takes priority; when it ends, the displays go back to
 * whatever the latest display message says.
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

  // Apply a payload from the display topic. Returns true if every display needs redrawing.
  bool apply(const char *payload);
  // Apply a payload from the countdown topic; an empty one cancels the countdown. Returns true if every display needs redrawing.
  bool applyCountdown(const char *payload);
  // Apply messages that were waiting for the time, follow the countdown, and end expired messages.
  // Returns true if every display needs redrawing.
  bool loop();

  // What's shown now: a running countdown, else the display topic's mode.
  modes getMode() { return countdown_active ? countdown : display_mode; }
  // Image name for a display position, counted from the left (0 to NUM_DIGITS - 1). Empty means black.
  const char *getTile(uint8_t position) { return countdown_active ? countdown_tiles[position] : tile_names[position]; }
  // Backlight colour as 0xRRGGBB for what's shown now, or -1 for the clock's own colour.
  int32_t getBacklight();
  // Backlight effect for what's shown now (a countdown's sweep or pulse), and its period in ms.
  Backlights::effects getEffect(uint16_t &period_ms);

private:
  // From the display topic
  modes display_mode = clock; // clock or tiles
  int64_t display_expires = 0; // Unix time (UTC); 0 means the message doesn't expire
  char tile_names[NUM_DIGITS][TILE_NAME_MAX_LEN + 1] = {};
  int32_t tiles_backlight = -1;

  // From the countdown topic
  bool countdown_active = false;
  int64_t countdown_expires = 0;
  int64_t until = 0; // Unix time (UTC) the countdown reaches zero
  uint8_t face = 1;  // Clock-face set whose digits are drawn
  char blank[TILE_NAME_MAX_LEN + 1] = {}; // Drawn instead of leading zero hours; empty: draw the zeros
  int32_t countdown_backlight = -1;
  bool sweep = false;        // Sweep the backlight back and forth while counting down
  uint16_t speedup = 0;      // Seconds before zero over which the sweep speeds up
  uint16_t pulse_after = 0;  // Seconds after zero that the backlight pulses
  int64_t tick_second = 0;   // The last whole second seen, and when: for timing within a second
  uint32_t tick_millis = 0;
  char countdown_tiles[NUM_DIGITS][TILE_NAME_MAX_LEN + 1] = {};

  // Messages that carry a time but arrived before the clock had one, applied once it has.
  char pending_display[DISPLAY_PAYLOAD_MAX_LEN] = {};
  char pending_countdown[DISPLAY_PAYLOAD_MAX_LEN] = {};

  int64_t millisLeft(); // Until zero, to the millisecond; negative after it
  void updateCountdown();
  static bool isValidTileName(const char *name);
  static bool parseBacklight(JsonVariant value, int32_t &rgb);
  static bool parseSeconds(JsonVariant value, uint16_t &seconds);
  enum time_check
  {
    time_ok,
    time_invalid,
    time_wait
  };
  static time_check checkExpires(JsonVariant value, int64_t &expires);
};

extern DisplayMode display_mode;

#endif // DISPLAY_MODE_H
