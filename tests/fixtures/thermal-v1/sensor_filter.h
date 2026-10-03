// Frozen median-3/Q8 IIR-3/8 methods from main 9569fcc.
class OldSensorFilter { public:
 double value() const { return filteredTempQ8_/2560.0; }
  void updateFilter(int16_t temp, int16_t hum) {
    tempWindow_[windowIndex_] = temp;
    humWindow_[windowIndex_] = hum;
    windowIndex_ = static_cast<uint8_t>((windowIndex_ + 1U) % 3U);
    if (windowCount_ < 3U) ++windowCount_;
    const int32_t targetTemp = medianTargetQ8(tempWindow_, windowCount_);
    const int32_t targetHum = medianTargetQ8(humWindow_, windowCount_);
    if (!filterInitialized_) {
      filteredTempQ8_ = targetTemp; filteredHumQ8_ = targetHum;
      filterInitialized_ = true; return;
    }
    filteredTempQ8_ += ((targetTemp - filteredTempQ8_) *
                        SHT485Config::IIR_NUMERATOR) / SHT485Config::IIR_DENOMINATOR;
    filteredHumQ8_ += ((targetHum - filteredHumQ8_) *
                       SHT485Config::IIR_NUMERATOR) / SHT485Config::IIR_DENOMINATOR;
  }
  static int32_t medianTargetQ8(const int16_t *v, uint8_t count) {
    if (count <= 1U) return static_cast<int32_t>(v[0]) * 256L;
    if (count == 2U) return (static_cast<int32_t>(v[0]) + v[1]) * 128L;
    const int16_t a = v[0], b = v[1], c = v[2];
    const int16_t m = (a > b) ? ((b > c) ? b : ((a > c) ? c : a))
                              : ((a > c) ? a : ((b > c) ? c : b));
    return static_cast<int32_t>(m) * 256L;
  }
private: int16_t tempWindow_[3]{}, humWindow_[3]{}; uint8_t windowIndex_=0,windowCount_=0;
 int32_t filteredTempQ8_=0,filteredHumQ8_=0; bool filterInitialized_=false;
};
