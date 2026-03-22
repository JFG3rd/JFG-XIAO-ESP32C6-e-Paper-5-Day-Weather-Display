#include "JD79661Panel.h"

namespace
{
constexpr uint8_t kCmdPanelSetting = 0x00;
constexpr uint8_t kCmdPowerSetting = 0x01;
constexpr uint8_t kCmdPowerOff = 0x02;
constexpr uint8_t kCmdBoosterSoftStart = 0x06;
constexpr uint8_t kCmdDeepSleep = 0x07;
constexpr uint8_t kCmdPowerOn = 0x04;
constexpr uint8_t kCmdDataStartTransmission = 0x10;
constexpr uint8_t kCmdDisplayRefresh = 0x12;
constexpr uint8_t kCmdPllControl = 0x30;
constexpr uint8_t kCmdVcomDataInterval = 0x50;
constexpr uint8_t kCmdTcon = 0x60;
constexpr uint8_t kCmdResolution = 0x61;
constexpr uint8_t kCmdPowerSave = 0xE3;
constexpr uint8_t kCmdMisc = 0xE7;
constexpr uint8_t kCmdUnknownB4 = 0xB4;
constexpr uint8_t kCmdUnknownB5 = 0xB5;
constexpr uint8_t kCmdUnknownE9 = 0xE9;
constexpr uint8_t kCmdUnknown4D = 0x4D;
}  // namespace

JD79661Panel::JD79661Panel(const Pins& pins) : pins_(pins) {}

bool JD79661Panel::begin()
{
  pinMode(pins_.cs, OUTPUT);
  pinMode(pins_.dc, OUTPUT);
  pinMode(pins_.rst, OUTPUT);
  pinMode(pins_.busy, INPUT);

  digitalWrite(pins_.cs, HIGH);
  digitalWrite(pins_.dc, HIGH);
  digitalWrite(pins_.rst, HIGH);

  // The panel is write-only in this setup. Passing MISO = -1 avoids colliding
  // with GPIO0, which is also commonly used as panel reset on the XIAO ESP32C6.
  SPI.begin(pins_.sck, -1, pins_.mosi, pins_.cs);
  return resetAndInit();
}

bool JD79661Panel::resetAndInit()
{
  hardwareReset();
  if (!waitUntilIdle("post-reset", false)) {
    return false;
  }

  writeCommand(kCmdUnknown4D);
  writeData(0x78);

  writeCommand(kCmdPanelSetting);
  writeData(0x0F);
  writeData(0x29);

  writeCommand(kCmdPowerSetting);
  writeData(0x07);
  writeData(0x00);

  writeCommand(0x03);
  writeData(0x10);
  writeData(0x54);
  writeData(0x44);

  writeCommand(kCmdBoosterSoftStart);
  writeData(0x05);
  writeData(0x00);
  writeData(0x3F);
  writeData(0x0A);
  writeData(0x25);
  writeData(0x12);
  writeData(0x1A);

  writeCommand(kCmdVcomDataInterval);
  writeData(0x37);

  writeCommand(kCmdTcon);
  writeData(0x02);
  writeData(0x02);

  writeCommand(kCmdResolution);
  writeData(0x00);
  writeData(0x80);
  writeData(0x01);
  writeData(0x28);

  writeCommand(kCmdMisc);
  writeData(0x1C);

  writeCommand(kCmdPowerSave);
  writeData(0x22);

  writeCommand(kCmdUnknownB4);
  writeData(0xD0);

  writeCommand(kCmdUnknownB5);
  writeData(0x03);

  writeCommand(kCmdUnknownE9);
  writeData(0x01);

  writeCommand(kCmdPllControl);
  writeData(0x08);

  writeCommand(kCmdPowerOn);
  if (!waitUntilIdle("power-on", true)) {
    return false;
  }

  initialized_ = true;
  return true;
}

bool JD79661Panel::clear(PixelColor color)
{
  uint8_t packed = 0;
  const uint8_t value = static_cast<uint8_t>(color);
  for (int shift = 6; shift >= 0; shift -= 2) {
    packed |= static_cast<uint8_t>(value << shift);
  }

  if (!initialized_) {
    return false;
  }

  writeCommand(kCmdDataStartTransmission);

  uint8_t chunk[64];
  memset(chunk, packed, sizeof(chunk));

  size_t remaining = kPackedFrameBytes;
  while (remaining > 0) {
    const size_t burst = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
    writeDataBuffer(chunk, burst);
    remaining -= burst;
  }

  return refresh();
}

bool JD79661Panel::writePackedFrame(const uint8_t* packed_frame, size_t length)
{
  if (!initialized_ || packed_frame == nullptr || length != kPackedFrameBytes) {
    return false;
  }

  writeCommand(kCmdDataStartTransmission);
  writeDataBuffer(packed_frame, length);
  return refresh();
}

bool JD79661Panel::writeMonochromeFrame(const uint8_t* mono_frame, size_t length, bool msb_first)
{
  if (!initialized_ || mono_frame == nullptr || length != kMonoFrameBytes) {
    return false;
  }

  writeCommand(kCmdDataStartTransmission);

  for (size_t index = 0; index < length; ++index) {
    const uint8_t src = mono_frame[index];
    const uint8_t upper = static_cast<uint8_t>((src >> 4) & 0x0F);
    const uint8_t lower = static_cast<uint8_t>(src & 0x0F);
    writeData(packNibble(upper, msb_first));
    writeData(packNibble(lower, msb_first));
  }

  return refresh();
}

bool JD79661Panel::refresh()
{
  if (!initialized_) {
    return false;
  }

  writeCommand(kCmdDisplayRefresh);
  return waitUntilIdle("refresh", true);
}

bool JD79661Panel::powerOff()
{
  if (!initialized_) {
    return false;
  }

  writeCommand(kCmdPowerOff);
  return waitUntilIdle("power-off", true);
}

void JD79661Panel::hibernate()
{
  if (!initialized_) {
    return;
  }

  powerOff();
  writeCommand(kCmdDeepSleep);
  writeData(0xA5);
  initialized_ = false;
}

bool JD79661Panel::waitUntilIdle(const char* phase, bool require_busy_low_seen)
{
  const uint32_t started = millis();
  bool saw_busy_low = false;

  Serial.print("BUSY check ");
  Serial.print(phase);
  Serial.print(": initial=");
  Serial.println(digitalRead(pins_.busy));

  while ((millis() - started) <= kBusyTimeoutMs) {
    const int state = digitalRead(pins_.busy);
    if (state == LOW) {
      saw_busy_low = true;
    }
    if (saw_busy_low && state == HIGH) {
      Serial.print("BUSY released during ");
      Serial.println(phase);
      return true;
    }
    if (!require_busy_low_seen && state == HIGH) {
      return true;
    }
    delay(10);
  }

  if (require_busy_low_seen && !saw_busy_low) {
    Serial.print("BUSY never asserted during ");
    Serial.println(phase);
  } else {
    Serial.print("Timeout waiting for BUSY to release during ");
    Serial.println(phase);
  }
  return false;
}

void JD79661Panel::hardwareReset()
{
  digitalWrite(pins_.rst, LOW);
  delay(40);
  digitalWrite(pins_.rst, HIGH);
  delay(50);
}

void JD79661Panel::writeCommand(uint8_t command)
{
  SPI.beginTransaction(SPISettings(kSpiClockHz, MSBFIRST, SPI_MODE0));
  digitalWrite(pins_.dc, LOW);
  digitalWrite(pins_.cs, LOW);
  SPI.transfer(command);
  digitalWrite(pins_.cs, HIGH);
  SPI.endTransaction();
}

void JD79661Panel::writeData(uint8_t data)
{
  SPI.beginTransaction(SPISettings(kSpiClockHz, MSBFIRST, SPI_MODE0));
  digitalWrite(pins_.dc, HIGH);
  digitalWrite(pins_.cs, LOW);
  SPI.transfer(data);
  digitalWrite(pins_.cs, HIGH);
  SPI.endTransaction();
}

void JD79661Panel::writeDataBuffer(const uint8_t* data, size_t length)
{
  SPI.beginTransaction(SPISettings(kSpiClockHz, MSBFIRST, SPI_MODE0));
  digitalWrite(pins_.dc, HIGH);
  digitalWrite(pins_.cs, LOW);
  for (size_t index = 0; index < length; ++index) {
    SPI.transfer(data[index]);
  }
  digitalWrite(pins_.cs, HIGH);
  SPI.endTransaction();
}

uint8_t JD79661Panel::packNibble(uint8_t nibble, bool msb_first) const
{
  uint8_t packed = 0;
  for (uint8_t bit = 0; bit < 4; ++bit) {
    const uint8_t source_shift = msb_first ? static_cast<uint8_t>(3 - bit) : bit;
    const bool pixel_is_white = ((nibble >> source_shift) & 0x1U) != 0;
    const uint8_t color = pixel_is_white
      ? static_cast<uint8_t>(PixelColor::White)
      : static_cast<uint8_t>(PixelColor::Black);
    packed |= static_cast<uint8_t>(color << ((3 - bit) * 2));
  }
  return packed;
}
