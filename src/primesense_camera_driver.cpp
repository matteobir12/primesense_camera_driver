#include "primesense_camera_driver.h"

// likely debug only
#include <iostream>

#include <stdexcept>
#include <cstdint>
#include <glob.h>
#include <utility>
#include <array>
#include <algorithm>

namespace PS1080 {
namespace {
constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 1> SUPPORTED_DEVS = {
    std::pair<std::uint16_t, std::uint16_t>{ 0x1d27 /* idVender */, 0x0609 /* idProduct */},
};

constexpr const char* USB_PATH = "/dev/bus/usb/**/*";

}

Driver::Driver() {
    glob_t usb_devs_glob;
    glob(USB_PATH, 0, NULL, &usb_devs_glob);

    for (int dev_idx = 0; dev_idx < usb_devs_glob.gl_pathc; dev_idx++) {
        char* const usb_dev_path = usb_devs_glob.gl_pathv[dev_idx];
        const auto dec = USBIO::probeUSBDescriptor(usb_dev_path);
        if (dec) {
            const auto found_dev_it = std::find_if(
                    SUPPORTED_DEVS.begin(),
                    SUPPORTED_DEVS.end(),
                    [&dec](const auto& dev_ids) { 
                        return dev_ids.first == dec->idVender && dev_ids.second == dec->idProduct; });

            if (found_dev_it != SUPPORTED_DEVS.end()) {
                fd = USBIO::openUSBDEV(usb_dev_path);
                break;
            }
        }
    }

    globfree(&usb_devs_glob);

    init();
}

Driver::Driver(const char* const handle) {
    fd = USBIO::openUSBDEV(handle);
    init();
}

void Driver::init() {
    if (fd < 0)
        throw std::runtime_error("Couldn't open USB dev");
    
    auto dev_desc = USBIO::probeUSBDescriptor(fd);
    if (!dev_desc || dev_desc->bcdUSB != 0x200 /*USB 2.0.0*/)
        throw std::runtime_error("Bad data from USB dev");

    auto* depth_conn_usb_buffer = new char[SENSOR_PROTOCOL_USB_BUFFER_SIZE];
    // DepthConnection.nUSBBufferReadOffset = 0;
    // DepthConnection.nUSBBufferWriteOffset = 0;

    std::cout << fetchStringFromST(1) << std::endl;

    auto* image_conn_usb_buffer = new char[SENSOR_PROTOCOL_USB_BUFFER_SIZE];
    // ImageConnection.nUSBBufferReadOffset = 0;
    // ImageConnection.nUSBBufferWriteOffset = 0;

    // looks like some cams support "misc" data, doubt mine does, but check later
}

std::string Driver::fetchStringFromST(const int indx) {
    unsigned char req_buff[64]{};
    USBIO::Transfer t_for_string;
    t_for_string.buffer = req_buff;
    t_for_string.usb_fd = fd;
    t_for_string.data_len = 64;
    t_for_string.endpoint = 0;
    t_for_string.type = USBIO::TransferType::CONTROL;

    std::cout << fd;
    char s_buffer[255];
    USBIO::transferForUSBString(&t_for_string, indx, s_buffer, 255);
    const auto r = USBIO::transfer(&t_for_string);
    if (r != USBIO::TransferError::SUCCESS)
        return "ERROR";

    // returned buffer is (char) data len, (char) desc (doesn't matter), (UCS-2) data
    const std::uint8_t data_len = s_buffer[0] - 2;

    // simple USC-2 to ASCII
    char asciiStr[data_len];
    for (std::uint8_t i = 0; i < data_len; i++)
        asciiStr[i] = s_buffer[(i * 2) + 2];

    return std::string(asciiStr);
}

}