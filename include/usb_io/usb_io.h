#pragma once

namespace USBIO {
enum class TransferType  {
    CONTROL,
    BULK,
    BULK_STREAM,
    INTERRUPT,
    ISOCHRONOUS
};

struct Transfer {
    TransferType type;
    // maybe a private lib thing
    int usb_fb = -1;

    int data_len;
    unsigned char *buffer;

    unsigned char endpoint;

    // callback?
};

int transfer(Transfer* const transfer);

void buffForStringDesc(void* const buffer);

}