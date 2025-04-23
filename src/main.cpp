#include <array>
#include <cstdio>
#include <cstring>

#include "primesense_camera_driver.h"

// test only 
constexpr const char* CAM_USB_HARDCODE = "/dev/bus/usb/001/012";

int main() {
  PS1080::Driver d(CAM_USB_HARDCODE);

  // Drivers loaded
  return 0;
}
