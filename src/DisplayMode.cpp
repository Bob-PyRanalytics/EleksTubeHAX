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
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
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

  // Times are absolute, so a retained message is still right after a reboot. Without the time, they can't be checked.
  int64_t new_expires = 0;
  JsonVariant expires_value = doc["expires"];
  if (!expires_value.isNull())
  {
    if (!expires_value.is<double>())
    {
      Serial.println("Display message ignored, expires isn't a number.");
      return false;
    }
    if (timeStatus() == timeNotSet)
    {
      Serial.println("Display message ignored, the time isn't set yet.");
      return false;
    }
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
    return redraw;
  }

  Serial.print("Display message ignored, unknown mode: ");
  Serial.println(new_mode);
  return false;
}

bool DisplayMode::loop()
{
  if (expires == 0 || timeStatus() == timeNotSet || int64_t(now()) < expires)
  {
    return false;
  }
  expires = 0;
  if (mode == clock)
  {
    return false;
  }
  Serial.println("Display message expired, back to the clock.");
  mode = clock;
  return true;
}
