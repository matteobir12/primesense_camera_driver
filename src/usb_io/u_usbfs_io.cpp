#include "usb_io/u_usbfs_io.h"

#include <cstdint>

#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>
#include <sys/errno.h>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <iostream>

namespace USBIO {
namespace {
// find real number
constexpr int MAX_CONTROL_TRANSFER_DATA_PAYLOAD = 4096;
TransferError control_transfer(Transfer* const transfer) {
	if (transfer->d.data_len - sizeof(usbdevfs_ctrltransfer) > MAX_CONTROL_TRANSFER_DATA_PAYLOAD)
		return TransferError::ERROR;
    
    // usbdevfs_urb* const urb = new usbdevfs_urb{};
	// urb->usercontext = transfer;
	// urb->type = USBDEVFS_URB_TYPE_CONTROL;
	// urb->endpoint = transfer->endpoint;
	// urb->buffer = transfer->buffer;
	// urb->buffer_length = transfer->data_len;

    const int r = ioctl(transfer->usb_fd, USBDEVFS_CONTROL, transfer->d.buffer);
    if (r < 0) {
        if (errno == ENODEV)
            return TransferError::ERROR;

        return TransferError::ERROR;
    }

    return TransferError::SUCCESS;
}



TransferError bulk_transfer(const Transfer* const transfer) {
    if (!transfer->d.buffer)
        return TransferError::ERROR;

    const int r = ioctl(transfer->usb_fd, USBDEVFS_BULK, transfer->d.buffer);
    if (r < 0)
        return TransferError::ERROR;


    return TransferError::SUCCESS;
}

// TODO
TransferError iso_transfer(const Transfer* const transfer) {
    return TransferError::ERROR;
}

}

std::optional<UsbDeviceDiscriptor> probeUSBDeviceDescriptor(const char* const usb_path) {
    const int fd = open(usb_path, O_RDWR);
    if (fd < 0)
        return std::nullopt;

    auto desc = probeUSBDeviceDescriptor(fd);
    
    close(fd);
    return desc;
}

std::optional<UsbDeviceDiscriptor> probeUSBDeviceDescriptor(const int fd) {
    UsbDeviceDiscriptor dev_desc;
    const auto read_size = read(fd, &dev_desc, sizeof(UsbDeviceDiscriptor));
    
    if (read_size != 18 || dev_desc.bLength != 18)
        return std::nullopt;

    return dev_desc;
}


// can use for managing internal private data
// TODO internal private data
int openUSBDEV(const char* const usb_dev_path) {
    const int fd = open(usb_dev_path, O_RDWR);

    return fd;
}

// very c like, maybe I prefer the idea of it being raii
void closeUSBDEV(const int fd) {
    fsync(fd);
    close(fd);
}

TransferError transfer(Transfer* const transfer) {
    if (!transfer || !transfer->d.valid)
        return TransferError::ERROR;

    switch (transfer->type) {
        case TransferType::CONTROL:
            return control_transfer(transfer);
        case TransferType::BULK:
        case TransferType::BULK_STREAM:
        case TransferType::INTERRUPT:
            return bulk_transfer(transfer);
        case TransferType::ISOCHRONOUS:
            return iso_transfer(transfer);
    }

    return TransferError::ERROR;
}

void transferForUSBString(Transfer* const transfer, const int index, const char* str_buff, const int str_buff_len) {
    auto* ctrl = (usbdevfs_ctrltransfer*) transfer->d.buffer;
    std::memset(ctrl, 0, sizeof(usbdevfs_ctrltransfer));
    transfer->d.data_len = sizeof(usbdevfs_ctrltransfer);

    ctrl->bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    ctrl->bRequest = USB_REQ_GET_DESCRIPTOR;
    ctrl->wValue = (USB_DT_STRING << 8) | index;  // index 1 (manufacturer)
    ctrl->wIndex = 0;
    ctrl->wLength = str_buff_len;
    ctrl->data = (void*) str_buff;

    transfer->d.valid = true;
}

void transferForConfigDescriptor(Transfer* const transfer, void* const buff, const int buff_len) {
    auto* ctrl = (usbdevfs_ctrltransfer*) transfer->d.buffer;
    std::memset(ctrl, 0, sizeof(usbdevfs_ctrltransfer));
    transfer->d.data_len = sizeof(usbdevfs_ctrltransfer);

    ctrl->bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    ctrl->bRequest = USB_REQ_GET_DESCRIPTOR;
    ctrl->wValue = (USB_DT_CONFIG << 8) | 0;  // index 1 (manufacturer)
    ctrl->wIndex = 0;
    ctrl->wLength = buff_len;
    ctrl->data = (void*) buff;

    transfer->d.valid = true;
};

void transferForBulk(Transfer* const transfer, const unsigned int ep, void* const buff, const int buff_len, const unsigned int timeout) {
    auto* bulk = (usbdevfs_bulktransfer*) transfer->d.buffer;
    std::memset(bulk, 0, sizeof(usbdevfs_bulktransfer));
    transfer->d.data_len = sizeof(usbdevfs_bulktransfer);

    bulk->ep = ep;
    bulk->timeout = timeout;
    bulk->len = buff_len;
    bulk->data = (void*) buff;

    transfer->d.valid = true;
}

std::optional<UsbDescriptorTree> getUSBDescriptorTree(const int fd) {
    USBIO::Transfer t;
    t.usb_fd = fd;
    t.endpoint = 0;
    t.type = USBIO::TransferType::CONTROL;

    unsigned char buff[512];
    std::memset(buff, 0, sizeof(buff));
    UsbConfigDiscriptor* d = (UsbConfigDiscriptor*) buff;
    transferForConfigDescriptor(&t, (void*) buff, 512);
    auto er = transfer(&t);

    if (er != TransferError::SUCCESS)
        return std::nullopt;
    
    UsbDescriptorTree out;

    size_t i = 0;
    ConfigurationDescriptor* current_config = nullptr;
    InterfaceDescriptor* current_iface = nullptr;
    // while valid descriptors
    while (i < sizeof(buff) && buff[i] >= 2) {
        uint8_t bLength = buff[i];
        uint8_t bDescriptorType = buff[i + 1];

        // don't walk off
        if (i + bLength > sizeof(buff))
            break;

        switch (bDescriptorType) {
            case 0x02: { // configuration
                ConfigurationDescriptor config = {};
                config.bConfigurationValue = buff[i + 5];
                config.bNumInterfaces = buff[i + 4];
                config.wTotalLength = buff[i + 2] | (buff[i + 3] << 8);
                out.configs.push_back(config);
                current_config = &out.configs.back();
                current_iface = nullptr;
                break;
            }

            case 0x04: { // interface
                InterfaceDescriptor iface = {};
                iface.bInterfaceNumber = buff[i + 2];
                iface.bAlternateSetting = buff[i + 3];
                iface.bNumEndpoints = buff[i + 4];
                iface.bInterfaceClass = buff[i + 5];
                iface.bInterfaceSubClass = buff[i + 6];
                iface.bInterfaceProtocol = buff[i + 7];
                if (current_config)
                    current_config->interfaces.push_back(iface);
                current_iface = current_config ? &current_config->interfaces.back() : nullptr;
                break;
            }

            case 0x05: { // endpoint
                if (!current_iface) break;
                EndpointDescriptor ep = {};
                ep.bEndpointAddress = buff[i + 2];
                ep.transfer_type = static_cast<TransferType>(buff[i + 3] & 0b00000011);
                ep.sync_type = static_cast<SyncType>(buff[i + 3] & 0b00001100);
                ep.usage_type = static_cast<UsageType>(buff[i + 3] & 0b00110000);
                ep.wMaxPacketSize = buff[i + 4] | (buff[i + 5] << 8);
                ep.bInterval = buff[i + 6];
                current_iface->endpoints.push_back(ep);
                break;
            }

            case 0x24: { // class specific interface descriptor
                if (!current_iface) break;
                ClassSpecificDescriptor cs = {};
                cs.bDescriptorSubtype = buff[i + 2];
                cs.raw_data.assign(buff + i, buff + i + bLength);
                current_iface->class_specific.push_back(cs);
                break;
            }

            case 0x25: { // class specific endpoint
                if (!current_iface || current_iface->endpoints.empty()) break;
                ClassSpecificDescriptor cs = {};
                cs.bDescriptorSubtype = buff[i + 2];
                cs.raw_data.assign(buff + i, buff + i + bLength);
                current_iface->endpoints.back().class_specific.push_back(cs);
                break;
            }
        }

        i += bLength;
    }

    return out;
}

}