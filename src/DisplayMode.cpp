#include <ArduinoJson.h>
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

  // Times are absolute, so a retained message is still right after a reboot. Without the time, they can't be
  // checked: a message that carries one waits until the clock has the time (loop()), unless another replaces it.
  JsonVariant expires_value = doc["expires"];
  JsonVariant until_value = doc["until"];
  if (!expires_value.isNull() && !expires_value.is<double>())
  {
    Serial.println("Display message ignored, expires isn't a number.");
    return false;
  }
  bool is_countdown = (strcmp(new_mode, "countdown") == 0);
  if ((!expires_value.isNull() || is_countdown) && timeStatus() == timeNotSet)
  {
    Serial.println("Display message waiting until the time is set.");
    strlcpy(pending, message, sizeof(pending));
    return false;
  }
  int64_t new_expires = 0;
  if (!expires_value.isNull())
  {
    new_expires = int64_t(expires_value.as<double>());
    if (new_expires <= int64_t(now()))
    {
      Serial.println("Display message ignored, it has already expired.");
      return false;
    }
  }

  if (strcmp(new_mode, "clock") == 0)
  {
    bool redraw = (mode != clock);
    mode = clock;
    expires = new_expires;
    pending[0] = '\0';
    return redraw;
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
    // Within tiles mode only the changed displays are redrawn; switching into it redraws them all.
    bool redraw = (mode != tiles);
    memcpy(tile_names, new_names, sizeof(tile_names));
    mode = tiles;
    expires = new_expires;
    pending[0] = '\0';
    return redraw;
  }

  if (is_countdown)
  {
    if (!until_value.is<double>())
    {
      Serial.println("Display message ignored, countdown needs until as a number.");
      return false;
    }
    JsonVariant face_value = doc["face"];
    if (!face_value.is<int>() || face_value.as<int>() < 1 || face_value.as<int>() > 99)
    {
      Serial.println("Display message ignored, countdown needs face as a whole number from 1 to 99.");
      return false;
    }
    JsonVariant blank_value = doc["blank"];
    if (!blank_value.isNull() && (!blank_value.is<const char *>() || !isValidTileName(blank_value.as<const char *>())))
    {
      Serial.println("Display message ignored, blank isn't a valid image name.");
      return false;
    }
    // Within countdown mode only the changed displays are redrawn; switching into it redraws them all.
    bool redraw = (mode != countdown);
    until = int64_t(until_value.as<double>());
    face = uint8_t(face_value.as<int>());
    strlcpy(blank, blank_value.isNull() ? "" : blank_value.as<const char *>(), sizeof(blank));
    mode = countdown;
    expires = new_expires;
    pending[0] = '\0';
    updateCountdown();
    return redraw;
  }

  Serial.print("Display message ignored, unknown mode: ");
  Serial.println(new_mode);
  return false;
}

bool DisplayMode::loop()
{
  if (timeStatus() == timeNotSet)
  {
    return false;
  }

  bool redraw = false;
  if (pending[0] != '\0')
  {
    Serial.println("The time is set: applying the waiting display message.");
    redraw = apply(pending);
    pending[0] = '\0';
  }

  if (expires != 0 && int64_t(now()) >= expires)
  {
    expires = 0;
    if (mode != clock)
    {
      Serial.println("Display message expired, back to the clock.");
      mode = clock;
      return true;
    }
  }

  if (mode == countdown)
  {
    updateCountdown(); // The displays whose digit changed are redrawn by the caller
  }
  return redraw;
}

// Six tiles H H M M S S from the time left, drawn with the face's digit files ("23" is digit 3 of face 2).
void DisplayMode::updateCountdown()
{
  const int64_t most = 99 * 3600 + 59 * 60 + 59; // Six displays: 99:59:59 at most
  int64_t left = until - int64_t(now());
  if (left < 0)
  {
    left = 0; // Hold at zero until the message expires or is replaced
  }
  if (left > most)
  {
    left = most;
  }
  uint8_t hours = left / 3600;
  uint8_t minutes = (left / 60) % 60;
  uint8_t seconds = left % 60;
  const uint8_t digits[NUM_DIGITS] = {uint8_t(hours / 10), uint8_t(hours % 10), uint8_t(minutes / 10),
                                      uint8_t(minutes % 10), uint8_t(seconds / 10), uint8_t(seconds % 10)};
  for (uint8_t position = 0; position < NUM_DIGITS; position++)
  {
    snprintf(tile_names[position], sizeof(tile_names[position]), "%u%u", face, digits[position]);
  }

  if (blank[0] != '\0')
  {
    if (hours < 10)
    {
      strlcpy(tile_names[0], blank, sizeof(tile_names[0]));
    }
    if (hours == 0)
    {
      strlcpy(tile_names[1], blank, sizeof(tile_names[1]));
    }
  }
}
