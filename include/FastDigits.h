#ifndef FAST_DIGITS_H
#define FAST_DIGITS_H

#include <stdint.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>

/*
 * One clock-face digit set (files "N0" to "N9") kept in a raw flash partition ("digits") and mapped
 * into memory, so a digit can be pushed to a display straight from flash. Reading a 135 x 240 image
 * through LittleFS takes about 65 ms; from the mapped partition it's just the 15 ms display push.
 * That makes a tenths-of-a-second countdown digit affordable.
 *
 * The partition is filled from the LittleFS files by the firmware itself, the first time a face is
 * asked for (and again if the files change), so there's no extra upload step.
 *
 * Partition layout: one 64 KB slot per image. Slot 0 holds the header, slots 1-10 digits 0-9 as raw
 * little-endian RGB565 (the .clk pixels without their 6-byte header).
 */
class FastDigits
{
public:
  // Make sure the partition holds this face's digits, checking it against the files once per boot
  // and rebuilding it if they differ. Can take a few seconds the first time. Returns true if ready.
  bool prepare(uint8_t face);
  // The pixels for a tile name if it's one of the prepared face's digits ("27" for face 2), else nullptr.
  const uint16_t *get(const char *name);

private:
  static const uint32_t slot_size = 0x10000;
  static const uint32_t magic = 0x31474446; // "FDG1"
  struct Header
  {
    uint32_t magic;
    uint8_t face;
    uint8_t reserved[3];
    uint16_t valid; // Bit n: digit n is present
  };

  const esp_partition_t *partition = nullptr;
  const uint8_t *mapped = nullptr;
  spi_flash_mmap_handle_t map_handle = 0;
  bool unavailable = false; // No partition, or it can't be mapped: don't keep trying
  uint8_t ready_face = 0;   // 0: nothing checked yet this boot
  uint8_t failed_face = 0;  // A face that couldn't be prepared this boot: don't keep trying
  uint16_t valid = 0;

  bool map();
  void unmap();
  bool matchesFiles(uint8_t face);
  bool rebuild(uint8_t face);
};

extern FastDigits fast_digits;

#endif // FAST_DIGITS_H
