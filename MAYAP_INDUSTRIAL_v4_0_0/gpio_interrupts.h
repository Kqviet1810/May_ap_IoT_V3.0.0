#pragma once
#include <driver/gpio.h>
#include <esp_intr_alloc.h>

// Install before any GPIO handlers. Arduino's default service/wrapper is not
// IRAM-safe in the pinned core. All handlers on this service must use RAM only.
inline bool mayapEnsureCacheSafeGpioService() {
  static bool installed = false;
  if (!installed) installed = gpio_install_isr_service(ESP_INTR_FLAG_IRAM) == ESP_OK;
  // Do not silently accept someone else's service with unknown interrupt flags.
  return installed;
}
