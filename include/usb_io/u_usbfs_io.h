#pragma once

#include <optional>
#include <cstdint>

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

namespace USBIO {
enum class TransferType  {
    CONTROL,
    BULK,
    BULK_STREAM,
    INTERRUPT,
    ISOCHRONOUS
};

enum class TransferError {
    SUCCESS,
    ERROR
};

struct Transfer {
    TransferType type;
    // maybe a private lib thing
    int usb_fd = -1;

    int data_len;
    unsigned char *buffer;

    unsigned char endpoint;

    // if urb callback?
};

// optional as error union, empty is error
std::optional<UsbDiscriptor> probeUSBDescriptor(const int fd);
std::optional<UsbDiscriptor> probeUSBDescriptor(const char* const usb_path);

int openUSBDEV(const char* const usb_dev_path);
void closeUSBDEV(const int fd);

TransferError transfer(Transfer* const transfer);

void transferForUSBString(Transfer* const tranfer, const int index, const char* str_buff, const int str_buff_len);

}