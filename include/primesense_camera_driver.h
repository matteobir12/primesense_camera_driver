// PS1080 Userspace USB Driver
#pragma once

#include "usb_io/usb_io.h"

// From OpenNI2, 4 chan 1024 x 1024 px
#define SENSOR_PROTOCOL_USB_BUFFER_SIZE 4 * 1024 * 1024

namespace PS1080 {
class Driver {
  public:
    // Search for usb device
    Driver();

    // Try handle as USB dev
    // Needs to be null terminated
    Driver(const char* const handle);

  private:
    void init();

    int fd = -1; // fd of USB dev or -1 if not init
};

}
