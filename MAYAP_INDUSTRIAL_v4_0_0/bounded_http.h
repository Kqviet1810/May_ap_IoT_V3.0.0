#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <string.h>

// HTTPClient::writeToStream handles both Content-Length and chunked bodies.
// Refuse oversized/unknown-length excess without ever growing a String.
class MayapHttpBodySink : public Stream {
 public:
  MayapHttpBodySink(char *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {
    if (capacity_) buffer_[0] = '\0';
  }
  size_t write(uint8_t byte) override { return write(&byte, 1U); }
  size_t write(const uint8_t *data, size_t length) override {
    if (!capacity_ || length > capacity_ - 1U - used_) { overflow_ = true; return 0U; }
    memcpy(buffer_ + used_, data, length); used_ += length; buffer_[used_] = '\0';
    return length;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  bool overflow() const { return overflow_; }
 private:
  char *buffer_;
  size_t capacity_, used_ = 0U;
  bool overflow_ = false;
};

inline bool mayapReadBoundedHttpBody(HTTPClient &http, char *buffer, size_t capacity) {
  if (!buffer || capacity < 2U) return false;
  buffer[0] = '\0';
  if (http.getSize() >= static_cast<int>(capacity)) return false;
  MayapHttpBodySink sink(buffer, capacity);
  const int received = http.writeToStream(&sink);
  return received >= 0 && !sink.overflow() && (http.getSize() < 0 || received == http.getSize());
}
