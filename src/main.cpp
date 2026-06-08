#include <csignal>

#include "primesense_camera_driver.h"

namespace {
PS1080::Driver* g_driver = nullptr;

void OnSignal(int) {
  if (g_driver)
    g_driver->requestStop();
}

}  // namespace

int main() {
  PS1080::Driver d;
  g_driver = &d;

  // exit cleanly so the sensor streams get turned off; killing the process
  // mid stream bricks the sensor until reboot
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  d.StreamRGBD();
  return 0;
}
