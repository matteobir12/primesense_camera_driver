#pragma once

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
    int usb_fb = -1;

    int data_len;
    unsigned char *buffer;

    unsigned char endpoint;

    // callback?
};

TransferError transfer(Transfer* const transfer);

void transferForUSBManufacturer(Transfer* const tranfer, const char* str_buff, const int str_buff_len);


}