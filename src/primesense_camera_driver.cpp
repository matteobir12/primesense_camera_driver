#include "primesense_camera_driver.h"

// likely debug only
#include <iostream>

#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <cstdint>

#include <linux/ioctl.h>

namespace PS1080 {
namespace {

// following naming convention from USB 2.0 doc
struct __attribute__((packed)) UsbDiscriptor {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint16_t bcdUSB;
    std::uint8_t bDeviceClass;
    std::uint8_t bDeviceSubClass;
    std::uint8_t bDeviceProtocol;
    std::uint8_t bMaxPacketSize0;
    std::uint16_t idVender;
    std::uint16_t idProduct;
    std::uint16_t bcdDevice;
    std::uint8_t iManufacturer;
    std::uint8_t iProduct;
    std::uint8_t iSerialNumber;
    std::uint8_t bNumConfigurations;
};

static_assert(sizeof(UsbDiscriptor) == 18);

}

Driver::Driver() {
    //TODO
    init();
}

Driver::Driver(const char* const handle) {
    fd = open(handle, O_RDWR);
    init();
}

void Driver::init() {
    if (fd < 0)
        throw std::runtime_error("Couldn't open USB dev");
    
    UsbDiscriptor dev_desc;
    const auto read_size = read(fd, &dev_desc, sizeof(UsbDiscriptor));
    
    if (read_size != 18)
        throw std::runtime_error("Unexpected discriptor size");

    if (dev_desc.bLength != 18 || dev_desc.bcdUSB  != 0x200 /*USB 2.0.0*/)
        throw std::runtime_error("Bad data from USB dev");
}

}