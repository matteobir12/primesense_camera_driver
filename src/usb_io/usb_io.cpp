#include "usb_io/usb_io.h"

#include <stdexcept>
#include <cstdint>

#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>
#include <sys/errno.h>

namespace USBIO {
namespace {
struct __attribute__((packed)) ControlRequest {
    // 0:4 for recipient
    // 5:6 for type
    // 7 for direction (see usb 2.0 docs)
    std::uint8_t bmRequestType;
    std::uint8_t  bRequest;
    std::uint16_t wValue;
    std::uint16_t wIndex;
    std::uint16_t wLength;
};

static_assert(sizeof(ControlRequest) == 8);

// find real number
constexpr int MAX_CONTROL_TRANSFER_DATA_PAYLOAD = 4096;
int control_transfer(Transfer* const transfer) {
	if (transfer->data_len - sizeof(ControlRequest) > MAX_CONTROL_TRANSFER_DATA_PAYLOAD)
		return -1; // or throw too big or smth... maybe write an error type?
    
    usbdevfs_urb* const urb = (usbdevfs_urb*) calloc(1, sizeof(usbdevfs_urb));
	urb->usercontext = transfer;
	urb->type = USBDEVFS_URB_TYPE_CONTROL;
	urb->endpoint = transfer->endpoint;
	urb->buffer = transfer->buffer;
	urb->buffer_length = transfer->data_len;

    const int r = ioctl(transfer->usb_fb, USBDEVFS_SUBMITURB, urb);
    if (r < 0) {
        free(urb);

        if (errno == ENODEV)
            return -1;

        throw std::runtime_error("submiturb failed, errno=" + errno);
    }

    free(urb); // DELETEME will need to hold onto urb for results
    return 0;
}

// TODO
int bulk_transfer(const Transfer* const transfer) {
    return -1;
}

// TODO
int iso_transfer(const Transfer* const transfer) {
    return -1;
}

}

int transfer(Transfer* const transfer) {
    if (!transfer)
        return -1;

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
        default:
            throw std::runtime_error("unknown transfer type" + static_cast<int>(transfer->type));
    }

    return -1;
}

void buffForStringDesc(void* const buffer) {
    auto* ctrl = (ControlRequest*) buffer;

    ctrl->bmRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    ctrl->bRequest = USB_REQ_GET_DESCRIPTOR;
    ctrl->wValue = (USB_DT_STRING << 8) | 1;  // index 1 (manufacturer)
    ctrl->wIndex = 0;
    ctrl->wLength = 255;
}

}