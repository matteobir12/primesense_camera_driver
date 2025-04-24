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

    USBIO::Transfer t_for_string;
    t_for_string.buffer = new unsigned char[64]{};
    t_for_string.usb_fb = fd;
    t_for_string.data_len = 64;
    t_for_string.endpoint = 0;
    t_for_string.type = USBIO::TransferType::CONTROL;

    char s_buffer[256];
    memset(s_buffer, 'a', 255);
    s_buffer[255] = '\0';
    USBIO::transferForUSBManufacturer(&t_for_string, s_buffer, 255);
    const auto r = USBIO::transfer(&t_for_string);
    if (r != USBIO::TransferError::SUCCESS)
        std::cout << "ERROR\n";

    delete[] t_for_string.buffer;

    // returned buffer is (char) data len, (char) desc (doesn't matter), (UCS-2) data
    for (int i = 0; i < sizeof(s_buffer); ++i) {
        printf("%02x ", (unsigned char) s_buffer[i]);
        if ((i + 1) % 16 == 0) {
            printf("\n");
        }
    }
    printf("\n");
    auto* depth_conn_usb_buffer = new char[SENSOR_PROTOCOL_USB_BUFFER_SIZE];
    // DepthConnection.nUSBBufferReadOffset = 0;
    // DepthConnection.nUSBBufferWriteOffset = 0;

    auto* image_conn_usb_buffer = new char[SENSOR_PROTOCOL_USB_BUFFER_SIZE];
    // ImageConnection.nUSBBufferReadOffset = 0;
    // ImageConnection.nUSBBufferWriteOffset = 0;

    // looks like some cams support "misc" data, doubt mine does, but check later
}

}