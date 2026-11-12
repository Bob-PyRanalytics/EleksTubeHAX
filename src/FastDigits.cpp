#include <Arduino.h>
#include <LittleFS.h>
#include "FastDigits.h"
#include "TFTs.h"

FastDigits fast_digits;

static const uint32_t pixel_bytes = TFT_WIDTH * TFT_HEIGHT * 2; // A full-size image, without the .clk header
static uint8_t chunk[4096];                                     // For reading files; static to spare the stack

bool FastDigits::map()
{
  if (mapped != nullptr)
  {
    return true;
  }
  if (partition == nullptr)
  {
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "digits");
    if (partition == nullptr || partition->size < 11 * slot_size)
    {
      Serial.println("Fast digits: no 'digits' partition, so digits are read from the filesystem.");
      unavailable = true;
      return false;
    }
  }
  const void *pointer = nullptr;
  esp_err_t error = esp_partition_mmap(partition, 0, partition->size, SPI_FLASH_MMAP_DATA, &pointer, &map_handle);
  if (error != ESP_OK)
  {
    Serial.print("Fast digits: can't map the partition: ");
    Serial.println(esp_err_to_name(error));
    unavailable = true;
    return false;
  }
  mapped = static_cast<const uint8_t *>(pointer);
  return true;
}

void FastDigits::unmap()
{
  if (mapped != nullptr)
  {
    spi_flash_munmap(map_handle);
    mapped = nullptr;
  }
}

// Opens "/<face><digit>.clk" and checks it's a full-size image; leaves the file at its pixels.
static bool openDigit(uint8_t face, uint8_t digit, fs::File &file)
{
  char filename[12];
  snprintf(filename, sizeof(filename), "/%u%u.clk", face, digit);
  file = LittleFS.open(filename, "r");
  if (!file)
  {
    return false;
  }
  uint8_t header[6];
  if (file.read(header, sizeof(header)) != sizeof(header) || header[0] != 'C' || header[1] != 'K' ||
      (header[2] | header[3] << 8) != TFT_WIDTH || (header[4] | header[5] << 8) != TFT_HEIGHT ||
      file.size() != sizeof(header) + pixel_bytes)
  {
    file.close();
    return false; // Missing, not a .clk, or not full size: drawn from the filesystem as usual
  }
  return true;
}

bool FastDigits::matchesFiles(uint8_t face)
{
  const Header *header = reinterpret_cast<const Header *>(mapped);
  if (header->magic != magic || header->face != face)
  {
    return false;
  }
  for (uint8_t digit = 0; digit < 10; digit++)
  {
    fs::File file;
    bool usable = openDigit(face, digit, file);
    bool stored = header->valid & (1 << digit);
    if (usable != stored)
    {
      if (usable)
        file.close();
      return false;
    }
    if (!usable)
    {
      continue;
    }
    const uint8_t *slot = mapped + (digit + 1) * slot_size;
    for (uint32_t done = 0; done < pixel_bytes;)
    {
      size_t got = file.read(chunk, min(uint32_t(sizeof(chunk)), pixel_bytes - done));
      if (got == 0 || memcmp(chunk, slot + done, got) != 0)
      {
        file.close();
        return false;
      }
      done += got;
    }
    file.close();
  }
  valid = header->valid;
  return true;
}

bool FastDigits::rebuild(uint8_t face)
{
  unmap(); // Map again afterwards, so nothing stale is read through the cache

  esp_err_t error = esp_partition_erase_range(partition, 0, 11 * slot_size);
  if (error != ESP_OK)
  {
    Serial.print("Fast digits: erase failed: ");
    Serial.println(esp_err_to_name(error));
    return false;
  }

  Header header = {};
  header.magic = magic;
  header.face = face;
  for (uint8_t digit = 0; digit < 10; digit++)
  {
    fs::File file;
    if (!openDigit(face, digit, file))
    {
      continue;
    }
    uint32_t slot = (digit + 1) * slot_size;
    bool ok = true;
    for (uint32_t done = 0; done < pixel_bytes && ok;)
    {
      size_t got = file.read(chunk, min(uint32_t(sizeof(chunk)), pixel_bytes - done));
      ok = got > 0 && esp_partition_write(partition, slot + done, chunk, got) == ESP_OK;
      done += got;
    }
    file.close();
    if (ok)
    {
      header.valid |= 1 << digit;
    }
  }
  // The header goes last, so an interrupted rebuild is never taken as complete.
  error = esp_partition_write(partition, 0, &header, sizeof(header));
  if (error != ESP_OK || !map())
  {
    Serial.println("Fast digits: rebuild failed.");
    return false;
  }
  valid = header.valid;
  return true;
}

bool FastDigits::prepare(uint8_t face)
{
  if (face == ready_face)
  {
    return valid != 0;
  }
  if (unavailable || face == 0 || face == failed_face || !map())
  {
    return false;
  }

  uint32_t start = millis();
  Serial.print("Fast digits: checking face ");
  Serial.print(face);
  Serial.println(" against its files...");
  if (!matchesFiles(face))
  {
    Serial.println("Fast digits: they differ, copying the face's digits into the partition...");
    if (!rebuild(face))
    {
      failed_face = face;
      ready_face = 0;
      valid = 0;
      return false;
    }
  }
  ready_face = face;
  Serial.print("Fast digits: ready in ");
  Serial.print(millis() - start);
  Serial.print(" ms, digits present: 0b");
  Serial.println(valid, BIN);
  return valid != 0;
}

const uint16_t *FastDigits::get(const char *name)
{
  if (ready_face == 0 || mapped == nullptr)
  {
    return nullptr;
  }
  // Exactly the face number followed by one digit, e.g. "27" for face 2 or "127" for face 12.
  char expected_prefix[4];
  int prefix_length = snprintf(expected_prefix, sizeof(expected_prefix), "%u", ready_face);
  if (strlen(name) != size_t(prefix_length) + 1 || strncmp(name, expected_prefix, prefix_length) != 0)
  {
    return nullptr;
  }
  char last = name[prefix_length];
  if (last < '0' || last > '9' || !(valid & (1 << (last - '0'))))
  {
    return nullptr;
  }
  return reinterpret_cast<const uint16_t *>(mapped + (last - '0' + 1) * slot_size);
}
