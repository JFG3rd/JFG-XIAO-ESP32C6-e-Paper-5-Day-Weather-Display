#pragma once

#include <Arduino.h>
#include <SPI.h>

class JD79661Panel
{
public:
  static constexpr uint16_t kWidth = 128;
  static constexpr uint16_t kHeight = 296;
  static constexpr uint32_t kPackedFrameBytes = (kWidth * kHeight) / 4;
  static constexpr uint32_t kMonoFrameBytes = (kWidth * kHeight) / 8;

  enum class PixelColor : uint8_t
  {
    Black = 0x0,
    White = 0x1,
    Yellow = 0x2,
    Red = 0x3,
  };

  struct Pins
  {
    int8_t cs;
    int8_t dc;
    int8_t rst;
    int8_t busy;
    int8_t sck;
    int8_t mosi;
  };

  explicit JD79661Panel(const Pins& pins);

  bool begin();
  bool resetAndInit();
  bool clear(PixelColor color);
  bool writePackedFrame(const uint8_t* packed_frame, size_t length);
  bool writeMonochromeFrame(const uint8_t* mono_frame, size_t length, bool msb_first = true);
  bool refresh();
  bool powerOff();
  void hibernate();

private:
  static constexpr uint32_t kBusyTimeoutMs = 20000;
  static constexpr uint32_t kSpiClockHz = 4000000;

  Pins pins_;
  bool initialized_ = false;

  bool waitUntilIdle(const char* phase, bool require_busy_low_seen);
  void hardwareReset();
  void writeCommand(uint8_t command);
  void writeData(uint8_t data);
  void writeDataBuffer(const uint8_t* data, size_t length);
  uint8_t packNibble(uint8_t nibble, bool msb_first) const;
};
