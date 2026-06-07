// PS1080 Userspace USB Driver
#pragma once

#include "usb_io/u_usbfs_io.h"
#include "ps1080_host_protocol.h"

#include <memory>
#include <string>
#include <vector>

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

    std::string fetchStringFromST(const int indx);
    void StreamDepth();

  private:
    void init();
    void initFirmware();

    struct InterfaceKey {
      std::uint8_t interface_num = -1;
      std::uint8_t alt_setting = -1;

      bool operator==(const InterfaceKey& rhs) const {
          return interface_num == rhs.interface_num && alt_setting == rhs.alt_setting; };
      struct Hasher {
        std::size_t operator()(const InterfaceKey& key) const noexcept {
          return static_cast<std::size_t>(key.interface_num) << 16 | key.alt_setting;
        }
      };
    };

    std::unordered_map<InterfaceKey, std::pair<USBIO::InterfaceForIso,
        std::vector<USBIO::EndpointDescriptor>>, InterfaceKey::Hasher>
        receive_endpoints_;
    std::unique_ptr<HostProtocol> protocol_;
    int fd = -1; // fd of USB dev or -1 if not init
};

}
