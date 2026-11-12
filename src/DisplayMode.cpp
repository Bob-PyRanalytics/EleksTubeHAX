#include <TimeLib.h>
#include "DisplayMode.h"

DisplayMode display_mode;

bool DisplayMode::isValidTileName(const char *name)
{
  size_t length = strlen(name);
  if (length == 0 || length > TILE_NAME_MAX_LEN)
  {
    return false;
  }
  for (size_t i = 0; i < length; i++)
  {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
    {
      return false;
    }
  }
  return true;
}

// Optional "#RRGGBB". Absent: -1 (the clock's own colour). Returns false if it's there but not valid.
bool DisplayMode::parseBacklight(JsonVariant value, int32_t &rgb)
{
  rgb = -1;
  if (value.isNull())
  {
    return true;
  }
  const char *text = value.is<const char *>() ? value.as<const char *>() : nullptr;
  if (text == nullptr || strlen(text) != 7 || text[0] != '#')
  {
    return false;
  }
  for (uint8_t i = 1; i < 7; i++)
  {
    if (!isxdigit((unsigned char)text[i]))
    {
      return false;
    }
  }
  rgb = int32_t(strtol(text + 1, nullptr, 16));
  return true;
}

// Optional whole number of seconds, 0 to 3600. Absent: 0. Returns false if it's there but not valid.
bool DisplayMode::parseSeconds(JsonVariant value, uint16_t &seconds)
{
  seconds = 0;
  if (value.isNull())
  {
    return true;
  }
  if (!value.is<int>() || value.as<int>() < 0 || value.as<int>() > 3600)
  {
    return false;
  }
  seconds = uint16_t(value.as<int>());
  return true;
}

// Optional "expires". Times are absolute, so a retained message is still right after a reboot;
// without the time they can't be checked, so the message has to wait.
DisplayMode::time_check DisplayMode::checkExpires(JsonVariant value, int64_t &expires)
{
  expires = 0;
  if (value.isNull())
  {
    return time_ok;
  }
  if (!value.is<double>())
  {
    Serial.println("MQTT display message ignored, expires isn't a number.");
    return time_invalid;
  }
  if (timeStatus() == timeNotSet)
  {
    return time_wait;
  }
  expires = int64_t(value.as<double>());
  if (expires <= int64_t(now()))
  {
    Serial.println("MQTT display message ignored, it has already expired.");
    return time_invalid;
  }
  return time_ok;
}

// Time until zero to the millisecond (negative after it). TimeLib only gives whole seconds, so this
// notes when each second starts; it's as precise as the main loop is frequent.
int64_t DisplayMode::millisLeft()
{
  int64_t second = int64_t(now());
  if (second != tick_second)
  {
    tick_second = second;
    tick_millis = millis();
  }
  uint32_t into_second = min(uint32_t(millis() - tick_millis), uint32_t(999));
  return (until - second) * 1000 - into_second;
}

// The countdown's backlight: a sweep back and forth while counting down, speeding up over the last
// `speedup` seconds, then a pulse for `pulse_after` seconds after zero.
Backlights::effects DisplayMode::getEffect(uint16_t &period_ms)
{
  const float slowest_pass_ms = 1500, fastest_pass_ms = 200; // One sweep pass, end to end
  const uint16_t pulse_ms = 600;
  period_ms = 0;
  if (!countdown_active || timeStatus() == timeNotSet)
  {
    return Backlights::no_effect;
  }
  int64_t left_ms = millisLeft();
  if (left_ms > 0)
  {
    if (!sweep)
    {
      return Backlights::no_effect;
    }
    float passes_per_s = 1000 / slowest_pass_ms;
    if (speedup > 0 && left_ms < int64_t(speedup) * 1000)
    { // Speed up steadily: passes per second rise in a straight line to the fastest at zero
      float progress = 1 - float(left_ms) / (speedup * 1000.0f);
      passes_per_s += (1000 / fastest_pass_ms - passes_per_s) * progress;
    }
    period_ms = uint16_t(1000 / passes_per_s);
    return Backlights::sweep_effect;
  }
  if (-left_ms < int64_t(pulse_after) * 1000)
  {
    period_ms = pulse_ms;
    return Backlights::pulse_effect;
  }
  return Backlights::no_effect;
}

int32_t DisplayMode::getBacklight()
{
  if (countdown_active)
  {
    return countdown_backlight;
  }
  return display_mode == tiles ? tiles_backlight : -1;
}

bool DisplayMode::apply(const char *payload)
{
  // Keep a copy: payload may be the pending buffer itself, which this can overwrite.
  char message[DISPLAY_PAYLOAD_MAX_LEN];
  strlcpy(message, payload, sizeof(message));

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, static_cast<const char *>(message)); // const: parse a copy, leave message intact
  if (error)
  {
    Serial.print("Display message ignored, not valid JSON: ");
    Serial.println(error.c_str());
    return false;
  }

  const char *new_mode = doc["mode"];
  if (new_mode == nullptr)
  {
    Serial.println("Display message ignored, no mode.");
    return false;
  }

  int64_t new_expires;
  time_check times = checkExpires(doc["expires"], new_expires);
  if (times == time_invalid)
  {
    return false;
  }
  if (times == time_wait)
  {
    Serial.println("Display message waiting until the time is set.");
    strlcpy(pending_display, message, sizeof(pending_display));
    return false;
  }

  modes before = getMode();

  if (strcmp(new_mode, "clock") == 0)
  {
    display_mode = clock;
    display_expires = new_expires;
    pending_display[0] = '\0';
    return getMode() != before;
  }

  if (strcmp(new_mode, "tiles") == 0)
  {
    JsonArray list = doc["tiles"];
    if (list.isNull() || list.size() != NUM_DIGITS)
    {
      Serial.println("Display message ignored, tiles needs exactly one entry per display.");
      return false;
    }
    char new_names[NUM_DIGITS][TILE_NAME_MAX_LEN + 1] = {};
    for (uint8_t position = 0; position < NUM_DIGITS; position++)
    {
      JsonVariant entry = list[position];
      if (entry.isNull())
      {
        continue; // null: a black display
      }
      if (!entry.is<const char *>() || !isValidTileName(entry.as<const char *>()))
      {
        Serial.print("Display message ignored, not a valid image name at position ");
        Serial.println(position + 1);
        return false;
      }
      strcpy(new_names[position], entry.as<const char *>());
    }
    int32_t new_backlight;
    if (!parseBacklight(doc["backlight"], new_backlight))
    {
      Serial.println("Display message ignored, backlight isn't a #RRGGBB colour.");
      return false;
    }
    // Within tiles mode only the changed displays are redrawn; switching into it redraws them all.
    memcpy(tile_names, new_names, sizeof(tile_names));
    tiles_backlight = new_backlight;
    display_mode = tiles;
    display_expires = new_expires;
    pending_display[0] = '\0';
    return getMode() != before;
  }

  Serial.print("Display message ignored, unknown mode: ");
  Serial.println(new_mode);
  return false;
}

bool DisplayMode::applyCountdown(const char *payload)
{
  char message[DISPLAY_PAYLOAD_MAX_LEN];
  strlcpy(message, payload, sizeof(message));
  modes before = getMode();

  // Empty: cancelled (publishing an empty retained message also deletes it from the broker).
  if (message[0] == '\0')
  {
    if (countdown_active)
    {
      Serial.println("Countdown cancelled.");
    }
    countdown_active = false;
    pending_countdown[0] = '\0';
    return getMode() != before;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, static_cast<const char *>(message));
  if (error)
  {
    Serial.print("Countdown message ignored, not valid JSON: ");
    Serial.println(error.c_str());
    return false;
  }

  JsonVariant until_value = doc["until"];
  if (!until_value.is<double>())
  {
    Serial.println("Countdown message ignored, until isn't a number.");
    return false;
  }
  JsonVariant face_value = doc["face"];
  if (!face_value.is<int>() || face_value.as<int>() < 1 || face_value.as<int>() > 99)
  {
    Serial.println("Countdown message ignored, face isn't a whole number from 1 to 99.");
    return false;
  }
  JsonVariant blank_value = doc["blank"];
  if (!blank_value.isNull() && (!blank_value.is<const char *>() || !isValidTileName(blank_value.as<const char *>())))
  {
    Serial.println("Countdown message ignored, blank isn't a valid image name.");
    return false;
  }
  int32_t new_backlight;
  if (!parseBacklight(doc["backlight"], new_backlight))
  {
    Serial.println("Countdown message ignored, backlight isn't a #RRGGBB colour.");
    return false;
  }
  JsonVariant tenths_value = doc["tenths"];
  if (!tenths_value.isNull() && !tenths_value.is<bool>())
  {
    Serial.println("Countdown message ignored, tenths must be true or false.");
    return false;
  }
  JsonVariant dot_value = doc["dot"];
  if (!dot_value.isNull() && (!dot_value.is<const char *>() || !isValidTileName(dot_value.as<const char *>())))
  {
    Serial.println("Countdown message ignored, dot isn't a valid image name.");
    return false;
  }
  JsonVariant sweep_value = doc["sweep"];
  if (!sweep_value.isNull() && !sweep_value.is<bool>())
  {
    Serial.println("Countdown message ignored, sweep must be true or false.");
    return false;
  }
  uint16_t new_speedup, new_pulse_after;
  if (!parseSeconds(doc["speedup"], new_speedup) || !parseSeconds(doc["pulse_after"], new_pulse_after))
  {
    Serial.println("Countdown message ignored, speedup and pulse_after must be whole seconds, 0 to 3600.");
    return false;
  }

  int64_t new_expires;
  time_check times = checkExpires(doc["expires"], new_expires);
  if (times == time_invalid)
  {
    return false;
  }
  if (times == time_wait || timeStatus() == timeNotSet) // A countdown always needs the time
  {
    Serial.println("Countdown message waiting until the time is set.");
    strlcpy(pending_countdown, message, sizeof(pending_countdown));
    return false;
  }

  until = int64_t(until_value.as<double>());
  face = uint8_t(face_value.as<int>());
  strlcpy(blank, blank_value.isNull() ? "" : blank_value.as<const char *>(), sizeof(blank));
  countdown_backlight = new_backlight;
  tenths = tenths_value.isNull() ? false : tenths_value.as<bool>();
  strlcpy(dot, dot_value.isNull() ? "" : dot_value.as<const char *>(), sizeof(dot));
  sweep = sweep_value.isNull() ? false : sweep_value.as<bool>();
  speedup = new_speedup;
  pulse_after = new_pulse_after;
  countdown_expires = new_expires;
  countdown_active = true;
  pending_countdown[0] = '\0';
  updateCountdown();
  // Within a countdown only the changed displays are redrawn; starting one redraws them all.
  return getMode() != before;
}

bool DisplayMode::loop()
{
  if (timeStatus() == timeNotSet)
  {
    return false;
  }

  bool redraw = false;
  if (pending_display[0] != '\0')
  {
    Serial.println("The time is set: applying the waiting display message.");
    redraw |= apply(pending_display);
    pending_display[0] = '\0';
  }
  if (pending_countdown[0] != '\0')
  {
    Serial.println("The time is set: applying the waiting countdown message.");
    redraw |= applyCountdown(pending_countdown);
    pending_countdown[0] = '\0';
  }

  modes before = getMode();
  int64_t time_now = int64_t(now());
  if (display_expires != 0 && time_now >= display_expires)
  {
    display_expires = 0;
    if (display_mode != clock)
    {
      Serial.println("Display message expired, back to the clock.");
      display_mode = clock;
    }
  }
  if (countdown_active && countdown_expires != 0 && time_now >= countdown_expires)
  {
    Serial.println("Countdown expired.");
    countdown_active = false;
  }

  if (countdown_active)
  {
    updateCountdown(); // The displays whose digit changed are redrawn by the caller
  }
  return redraw || getMode() != before;
}

// Six tiles from the time left, drawn with the face's digit files ("23" is digit 3 of face 2):
// H H M M S S, or with tenths under an hour M M S S . t, the tenths stepping by two (8, 6, 4, 2, 0).
void DisplayMode::updateCountdown()
{
  const int64_t most_ms = (99 * 3600 + 59 * 60 + 59) * 1000LL; // Six displays: 99:59:59 at most
  int64_t left_ms = millisLeft();
  if (left_ms < 0)
  {
    left_ms = 0; // Hold at zero until the message expires or is replaced
  }
  if (left_ms > most_ms)
  {
    left_ms = most_ms;
  }

  uint8_t digits[NUM_DIGITS];
  bool tenths_layout = tenths && left_ms < 3600 * 1000LL;
  uint8_t hours = 0;
  if (tenths_layout)
  {
    uint32_t left_tenths = left_ms / 100;
    uint8_t minutes = left_tenths / 600;
    uint8_t seconds = (left_tenths / 10) % 60;
    uint8_t tenth = (left_tenths % 10) & ~1; // Every 2nd tenth: one draw every 200 ms
    const uint8_t layout[NUM_DIGITS] = {uint8_t(minutes / 10), uint8_t(minutes % 10), uint8_t(seconds / 10),
                                        uint8_t(seconds % 10), 0, tenth};
    memcpy(digits, layout, sizeof(digits));
  }
  else
  {
    // Whole seconds, rounded up, as before tenths: the display reaches 0 exactly at zero.
    int64_t left = (left_ms + 999) / 1000;
    hours = left / 3600;
    uint8_t minutes = (left / 60) % 60;
    uint8_t seconds = left % 60;
    const uint8_t layout[NUM_DIGITS] = {uint8_t(hours / 10), uint8_t(hours % 10), uint8_t(minutes / 10),
                                        uint8_t(minutes % 10), uint8_t(seconds / 10), uint8_t(seconds % 10)};
    memcpy(digits, layout, sizeof(digits));
  }
  for (uint8_t position = 0; position < NUM_DIGITS; position++)
  {
    snprintf(countdown_tiles[position], sizeof(countdown_tiles[position]), "%u%u", face, digits[position]);
  }

  if (tenths_layout)
  {
    strlcpy(countdown_tiles[4], dot, sizeof(countdown_tiles[4])); // Empty: black
  }
  else if (blank[0] != '\0')
  {
    if (hours < 10)
    {
      strlcpy(countdown_tiles[0], blank, sizeof(countdown_tiles[0]));
    }
    if (hours == 0)
    {
      strlcpy(countdown_tiles[1], blank, sizeof(countdown_tiles[1]));
    }
  }
}
