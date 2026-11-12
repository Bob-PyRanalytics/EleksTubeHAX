#include "Backlights.h"

void Backlights::begin(StoredConfig::Config::Backlights *config_)
{
  Adafruit_NeoPixel::begin(); // Initialize RMT and pin
  config = config_;

  if (config->is_valid != StoredConfig::valid)
  {
    // Config is invalid, probably a new device never had its config written.
    // Load some reasonable defaults.
    Serial.println("Loaded Backlights config is invalid, using default.  This is normal on first boot.");
    setPattern(rainbow);
    setColorPhase(0);
    setIntensity(max_intensity - 1);
    setPulseRate(60);
    setBreathRate(20);
    setRainbowDuration(DEFAULT_BL_RAINBOW_DURATION_SEC);
    config->is_valid = StoredConfig::valid;
  }
  off = false;
}

// These feel like they should be generalizable into a helper function.
// https://stackoverflow.com/questions/11720656/modulo-operation-with-negative-numbers
void Backlights::setNextPattern(int8_t i)
{
  int8_t next_pattern = (config->pattern + i) % num_patterns;
  while (next_pattern < 0)
  {
    next_pattern += num_patterns;
  }
  setPattern(patterns(next_pattern));
}

void Backlights::adjustColorPhase(int16_t adj)
{
  int16_t new_phase = (int16_t(config->color_phase % max_phase) + adj) % max_phase;
  while (new_phase < 0)
  {
    new_phase += max_phase;
  }
  setColorPhase(new_phase);
}

// The colour wheel cross-fades two channels at a time, so it can't show every RGB colour.
// Pick the phase whose colour points the most nearly the same way as the requested one.
void Backlights::setColor(uint32_t rgb)
{
  int16_t phase = colorToPhase(rgb);
  if (phase >= 0)
  {
    setColorPhase(phase); // Black has no hue (-1): leave the colour as it is
  }
}

void Backlights::setOverrideColor(uint32_t rgb)
{
  int16_t phase = colorToPhase(rgb);
  if (phase != override_phase)
  {
    override_phase = phase; // -1 for black: no override
    pattern_needs_init = true;
  }
}

void Backlights::clearOverrideColor()
{
  if (override_phase >= 0)
  {
    override_phase = -1;
    pattern_needs_init = true;
  }
}

int16_t Backlights::colorToPhase(uint32_t rgb)
{
  float r = (rgb >> 16) & 0xFF;
  float g = (rgb >> 8) & 0xFF;
  float b = rgb & 0xFF;
  float length = sqrtf(r * r + g * g + b * b);
  if (length == 0)
  {
    return -1;
  }

  uint16_t best_phase = 0;
  float best_match = -1;
  for (uint16_t phase = 0; phase < max_phase; phase++)
  {
    uint32_t color = phaseToColor(phase);
    float pr = (color >> 16) & 0xFF;
    float pg = (color >> 8) & 0xFF;
    float pb = color & 0xFF;
    float phase_length = sqrtf(pr * pr + pg * pg + pb * pb);
    if (phase_length == 0)
    {
      continue;
    }
    float match = (r * pr + g * pg + b * pb) / (length * phase_length);
    if (match > best_match)
    {
      best_match = match;
      best_phase = phase;
    }
  }
  return best_phase;
}

void Backlights::adjustIntensity(int16_t adj)
{
  int16_t new_intensity = (int16_t(config->intensity) + adj) % max_intensity;
  while (new_intensity < 0)
  {
    new_intensity += max_intensity;
  }
  setIntensity(new_intensity);
}

void Backlights::setIntensity(uint8_t intensity)
{
  config->intensity = intensity;
  setBrightness(0xFF >> max_intensity - config->intensity - 1);
  pattern_needs_init = true;
}

uint8_t Backlights::getEffectiveIntensity()
{
  return dimming ? (uint8_t)BACKLIGHT_DIMMED_INTENSITY : config->intensity;
}

uint32_t Backlights::scaleColor(uint32_t color, uint8_t level)
{
  uint32_t r = ((color >> 16) & 0xFF) * level / 255;
  uint32_t g = ((color >> 8) & 0xFF) * level / 255;
  uint32_t b = (color & 0xFF) * level / 255;
  return (r << 16) | (g << 8) | b;
}

// An effect replaces the pattern while it's on. Returns true if it drew the LEDs.
bool Backlights::effectLoop()
{
  if (effect == no_effect || off || effect_period_ms == 0)
  {
    if (effect_drawn != no_effect)
    {
      effect_drawn = no_effect;
      pattern_needs_init = true; // Put the pattern back
    }
    return false;
  }

  uint32_t time_now = millis();
  if (effect != effect_drawn)
  {
    effect_drawn = effect;
    effect_cycle = 0;
    effect_last_ms = time_now;
  }
  // A sweep's cycle is there and back: two passes.
  float cycle_ms = (effect == sweep_effect) ? 2.0f * effect_period_ms : float(effect_period_ms);
  effect_cycle += float(time_now - effect_last_ms) / cycle_ms;
  effect_cycle -= floorf(effect_cycle);
  effect_last_ms = time_now;

  // Base level: the normal brightness for the intensity setting. Bright: up to three times that,
  // and at least twice the base (so at full intensity the base comes down instead).
  uint16_t normal = 0xFF >> (max_intensity - getEffectiveIntensity() - 1);
  uint8_t bright = min(uint16_t(255), uint16_t(normal * 3));
  uint8_t base = min(normal, uint16_t(bright / 2));
  uint32_t color = phaseToColor(activePhase());
  uint16_t count = numPixels();

  setBrightness(255); // Levels are applied per LED below
  for (uint16_t led = 0; led < count; led++)
  {
    float amount; // 0: base, 1: bright
    if (effect == sweep_effect)
    {
      float there_and_back = effect_cycle < 0.5f ? effect_cycle * 2 : 2 - effect_cycle * 2;
      float spot = there_and_back * (count - 1);
      const float width = 1.5f; // LEDs either side of the spot that still light up a little
      amount = max(0.0f, 1 - fabsf(led - spot) / width);
    }
    else
    {
      amount = 0.5f - 0.5f * cosf(2 * M_PI * effect_cycle);
    }
    setPixelColor(led, scaleColor(color, uint8_t(base + (bright - base) * amount)));
  }
  show();
  pattern_needs_init = false;
  return true;
}

void Backlights::loop()
{
  if (effectLoop())
  {
    return;
  }

  //   enum patterns { dark, test, constant, rainbow, pulse, breath, num_patterns };
  if (off || config->pattern == dark)
  {
    if (pattern_needs_init)
    {
      clear();
      show();
    }
  }
  else if (config->pattern == test)
  {
    testPattern();
  }
  else if (config->pattern == constant)
  {
    if (pattern_needs_init)
    {
      fill(phaseToColor(activePhase()));
    }
    if (dimming)
    {
      setBrightness(0xFF >> max_intensity - BACKLIGHT_DIMMED_INTENSITY - 1);
    }
    else
    {
      setBrightness(0xFF >> max_intensity - config->intensity - 1);
    }
    show();
  }
  else if (config->pattern == rainbow)
  {
    rainbowPattern();
  }
  else if (config->pattern == pulse)
  {
    pulsePattern();
  }
  else if (config->pattern == breath)
  {
    breathPattern();
  }

  pattern_needs_init = false;
}

void Backlights::pulsePattern()
{
  fill(phaseToColor(activePhase()));

  float pulse_length_millis = (60.0f * 1000) / config->pulse_bpm;
  float val = 1 + abs(sin(2 * M_PI * millis() / pulse_length_millis)) * 254;
  if (dimming)
  {
    val = val * BACKLIGHT_DIMMED_INTENSITY / 7;
  }
  else
  {
    val = val * config->intensity / 7;
  }
  setBrightness((uint8_t)val);

  show();
}

void Backlights::breathPattern()
{
  fill(phaseToColor(activePhase()));

  // https://sean.voisen.org/blog/2011/10/breathing-led-with-arduino/
  // Avoid a 0 value as it shuts off the LEDs and we have to re-initialize.
  float pulse_length_millis = (60.0f * 1000) / config->breath_per_min;
  float val = (exp(sin(2 * M_PI * millis() / pulse_length_millis)) - 0.36787944f) * 108.0f;

  if (dimming)
  {
    val = val * BACKLIGHT_DIMMED_INTENSITY / 7;
  }
  else
  {
    val = val * config->intensity / 7;
  }

  uint8_t brightness = (uint8_t)val;
  if (brightness < 1)
  {
    brightness = 1;
  }
  setBrightness(brightness);

  show();
}

void Backlights::testPattern()
{
  const uint8_t num_colors = 4; // or 3 if you don't want black
  uint8_t num_states = NUM_BACKLIGHT_LEDS * num_colors;
  uint8_t state = (millis() / test_ms_delay) % num_states;

  uint8_t digit = state / num_colors;
  uint32_t color = 0xFF0000 >> (state % num_colors) * 8;

  clear();
  setPixelColor(digit, color);

  if (dimming)
  {
    setBrightness(0xFF >> max_intensity - (uint8_t)BACKLIGHT_DIMMED_INTENSITY - 1);
  }
  else
  {
    setBrightness(0xFF >> max_intensity - config->intensity - 1);
  }

  show();
}

uint8_t Backlights::phaseToIntensity(uint16_t phase)
{
  uint16_t color = 0;
  if (phase <= 255)
  {
    // Ramping up
    color = phase;
  }
  else if (phase <= 511)
  {
    // Ramping down
    color = 511 - phase;
  }
  else
  {
    // Off
    color = 0;
  }
  if (color > 255)
  {
    // TODO: Trigger ERROR STATE, bug in code.
  }
  return uint8_t(color % 256);
}

uint32_t Backlights::phaseToColor(uint16_t phase)
{
  uint8_t red = phaseToIntensity(phase);
  uint8_t green = phaseToIntensity((phase + 256) % max_phase);
  uint8_t blue = phaseToIntensity((phase + 512) % max_phase);
  return (uint32_t(red) << 16 | uint32_t(green) << 8 | uint32_t(blue));
}

uint32_t Backlights::hueToPhase(float hue)
{
  hue = hue - 120.f;
  if (hue < 0)
  {
    hue = hue + 360.f;
  }
  uint32_t phase = uint32_t(round(768.f * (1.f - hue / 360.f)));
  phase = phase % max_phase;
  return (phase);
}

float Backlights::phaseToHue(uint32_t phase)
{
  float hue = 120.f + ((768.f - float(phase)) / 768.f) * 360.f;
  // h = 120 + (1 - p/768)*360
  if (hue >= 360.f)
  {
    hue = hue - 360.f;
  }
  return (round(hue));
}

void Backlights::rainbowPattern()
{
  // Divide by 3 to spread it out some, so the whole rainbow isn't displayed at once.
  // TODO Make this /3 a parameter
  const uint16_t phase_per_digit = (max_phase / NUM_BACKLIGHT_LEDS) / 3;

  // Rainbow roatation speed now configurable
  uint16_t duration = uint16_t(round(getRainbowDuration() * 1000));
  uint16_t phase = uint16_t(round(float(millis() % duration) / duration * max_phase));

  for (uint8_t digit = 0; digit < NUM_BACKLIGHT_LEDS; digit++)
  {
    // Shift the phase for this LED.
    uint16_t my_phase = (phase + digit * phase_per_digit) % max_phase;
    setPixelColor(digit, phaseToColor(my_phase));
  }
  if (dimming)
  {
#if BACKLIGHT_DIMMED_INTENSITY > 0
    setBrightness(0xFF >> max_intensity - (uint8_t)BACKLIGHT_DIMMED_INTENSITY - 1);
#else // turn off backlight if intensity is 0
    setBrightness(0);
#endif
  }
  else
  {
    setBrightness(0xFF >> max_intensity - config->intensity - 1);
  }
  show();
}

const String Backlights::patterns_str[Backlights::num_patterns] =
    {"Dark", "Test", "Constant", "Rainbow", "Pulse", "Breath"};
