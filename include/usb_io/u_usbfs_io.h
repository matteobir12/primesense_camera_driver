#pragma once

#include <optional>
#include <cstdint>
#include <vector>

// following naming convention from USB 2.0 doc
struct __attribute__((packed)) UsbDeviceDiscriptor {
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

static_assert(sizeof(UsbDeviceDiscriptor) == 18);

struct __attribute__((packed)) UsbConfigDiscriptor {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint16_t wTotalLength;
    std::uint8_t bNumInterfaces;
    std::uint8_t bConfigurationValue;
    std::uint8_t iConfiguration;
    std::uint8_t bmAttributes;
    std::uint16_t bMaxPower;
};

static_assert(sizeof(UsbConfigDiscriptor) == 10);

struct __attribute__((packed)) UsbInterfaceDiscriptor {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bInterfaceNumber;
    std::uint8_t bAlternateSetting;
    std::uint8_t bNumEndpoints;
    std::uint8_t bInterfaceClass;
    std::uint8_t bInterfaceSubClass;
    std::uint8_t bInterfaceProtocol;
    std::uint8_t iInterface;

};

static_assert(sizeof(UsbInterfaceDiscriptor) == 9);

namespace USBIO {
struct ClassSpecificDescriptor {
    uint8_t bDescriptorSubtype;
    std::vector<uint8_t> raw_data;
};

enum class TransferType  {
    CONTROL = 0,
    BULK,
    ISOCHRONOUS,
    INTERRUPT,
    BULK_STREAM
};

enum class SyncType  {
    NO_SYNC = 0,
    ADAPTIVE,
    ASYNC,
    SYNC
};  

enum class UsageType  {
    DATA_ENDPOINT,
    IMPLICIT_FEEDBACK_DATA_ENDPOINT, // ??
    FEEDBACK_ENDPOINT,
    RESERVED // ??
};  

struct EndpointDescriptor {
    uint8_t bEndpointAddress;
    TransferType transfer_type;
    SyncType sync_type;
    UsageType usage_type;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    std::vector<ClassSpecificDescriptor> class_specific;
};

struct InterfaceDescriptor {
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    std::vector<EndpointDescriptor> endpoints;
    std::vector<ClassSpecificDescriptor> class_specific;
};

struct ConfigurationDescriptor {
    uint8_t bConfigurationValue;
    uint8_t bNumInterfaces;
    uint16_t wTotalLength;
    std::vector<InterfaceDescriptor> interfaces;
};

struct UsbDescriptorTree {
    std::vector<ConfigurationDescriptor> configs;
};

enum class TransferError {
    SUCCESS,
    ERROR
};


struct TransferData {
    int data_len = 64;
    bool valid = false;
    unsigned char buffer[64];
};

struct Transfer {
    TransferType type;
    // maybe a private lib thing
    int usb_fd = -1;
    unsigned char endpoint;

    // Opaque, set with 'transferFor' lib funcs
    TransferData d;
};

// optional as error union, empty is error
std::optional<UsbDeviceDiscriptor> probeUSBDeviceDescriptor(const int fd);
std::optional<UsbDeviceDiscriptor> probeUSBDeviceDescriptor(const char* const usb_path);
std::optional<UsbDescriptorTree> getUSBDescriptorTree(const int fd);


int openUSBDEV(const char* const usb_dev_path);
void closeUSBDEV(const int fd);

TransferError transfer(Transfer* const transfer);

void transferForUSBString(Transfer* const transfer, const int index, const char* str_buff, const int str_buff_len);
void transferForConfigDescriptor(Transfer* const transfer, const char* buff, const int buff_len = sizeof(UsbConfigDiscriptor));
void transferForBulk(Transfer* const transfer, const unsigned int ep, void* const buff, const int buff_len, const unsigned int timeout = 100);

}