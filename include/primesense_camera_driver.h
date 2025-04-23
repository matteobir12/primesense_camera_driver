// PS1080 Userspace USB Driver
#pragma once

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
