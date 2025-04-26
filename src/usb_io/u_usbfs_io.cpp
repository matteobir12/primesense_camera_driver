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
	if (transfer->data_len - sizeof(usbdevfs_ctrltransfer) > MAX_CONTROL_TRANSFER_DATA_PAYLOAD)
		return TransferError::ERROR;
    
    // usbdevfs_urb* const urb = new usbdevfs_urb{};
	// urb->usercontext = transfer;
	// urb->type = USBDEVFS_URB_TYPE_CONTROL;
	// urb->endpoint = transfer->endpoint;
	// urb->buffer = transfer->buffer;
	// urb->buffer_length = transfer->data_len;

    const int r = ioctl(transfer->usb_fd, USBDEVFS_CONTROL, transfer->buffer);
    if (r < 0) {
        if (errno == ENODEV)
            return TransferError::ERROR;

        return TransferError::ERROR;
    }

    return TransferError::SUCCESS;
}

// TODO
TransferError bulk_transfer(const Transfer* const transfer) {
    return TransferError::ERROR;
}

// TODO
TransferError iso_transfer(const Transfer* const transfer) {
    return TransferError::ERROR;
}

}

std::optional<UsbDiscriptor> probeUSBDescriptor(const char* const usb_path) {
    const int fd = open(usb_path, O_RDWR);
    if (fd < 0)
        return std::nullopt;

    auto desc = probeUSBDescriptor(fd);
    
    close(fd);
    return desc;
}

std::optional<UsbDiscriptor> probeUSBDescriptor(const int fd) {
    UsbDiscriptor dev_desc;
    const auto read_size = read(fd, &dev_desc, sizeof(UsbDiscriptor));
    
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
    close(fd);
}

TransferError transfer(Transfer* const transfer) {
    if (!transfer)
        return TransferError::ERROR;

    switch (transfer->type) {
        case TransferType::CONTROL:
            return control_transfer(transfer);
        case TransferType::BULK:
        case TransferType::BULK_STREAM:
            return bulk_transfer(transfer);
        case TransferType::INTERRUPT:
            return bulk_transfer(transfer);
        case TransferType::ISOCHRONOUS:
            return iso_transfer(transfer);
    }

    return TransferError::ERROR;
}

void transferForUSBString(Transfer* const tranfer, const int index, const char* str_buff, const int str_buff_len) {
    auto* ctrl = (usbdevfs_ctrltransfer*) tranfer->buffer;
    std::memset(ctrl, 0, sizeof(usbdevfs_ctrltransfer));
    tranfer->data_len = sizeof(usbdevfs_ctrltransfer);

    ctrl->bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    ctrl->bRequest = USB_REQ_GET_DESCRIPTOR;
    ctrl->wValue = (USB_DT_STRING << 8) | index;  // index 1 (manufacturer)
    ctrl->wIndex = 0;
    ctrl->wLength = str_buff_len;
    ctrl->data = (void*) str_buff;
}

}