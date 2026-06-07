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

TransferError iso_transfer(const Transfer* const transfer) {
    if (!transfer->d.buffer)
        return TransferError::ERROR;

    const int r = ioctl(transfer->usb_fd, USBDEVFS_SUBMITURB, transfer->d.buffer);
    if (r < 0)
        return TransferError::ERROR;

    return TransferError::SUCCESS;
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

int controlTransfer(
    const int fd, const uint8_t bRequestType, const uint8_t bRequest,
    const uint16_t wValue, const uint16_t wIndex,
    void* const data, const uint16_t wLength,
    const uint32_t timeout_ms)
{
  usbdevfs_ctrltransfer ctrl;
  std::memset(&ctrl, 0, sizeof(ctrl));

  ctrl.bRequestType = bRequestType;
  ctrl.bRequest = bRequest;
  ctrl.wValue = wValue;
  ctrl.wIndex = wIndex;
  ctrl.wLength = wLength;
  ctrl.timeout = timeout_ms;
  ctrl.data = data;

  const int r = ioctl(fd, USBDEVFS_CONTROL, &ctrl);
  if (r < 0)
      return -errno;

  return r;
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

void transferForConfigDescriptor(Transfer* const transfer, unsigned char* const buff, const int buff_len) {
    auto* ctrl = (usbdevfs_ctrltransfer*) transfer->d.buffer;
    std::memset(ctrl, 0, sizeof(usbdevfs_ctrltransfer));
    transfer->d.data_len = sizeof(usbdevfs_ctrltransfer);

    ctrl->bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    ctrl->bRequest = USB_REQ_GET_DESCRIPTOR;
    ctrl->wValue = (USB_DT_CONFIG << 8) | 0;  // config descriptor, index 0
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

// void transferForIsochronous(Transfer* const transfer, const EndpointDescriptor& ep, void* const buff, const int buff_len) {
//     usbdevfs_urb* const urb = (usbdevfs_urb*) transfer->d.buffer;
//     std::memset(urb, 0, sizeof(usbdevfs_urb));

//     const int packet_size = ep.wMaxPacketSize;
//     const int num_packets = buff_len / packet_size;

//     urb->usercontext = (void*) transfer;
//     urb->type = USBDEVFS_URB_TYPE_ISO;
//     urb->endpoint = ep.bEndpointAddress;
//     urb->buffer = buff;
//     urb->buffer_length = buff_len;
//     urb->number_of_packets = num_packets;

//     transfer->d.data_len = sizeof(usbdevfs_urb);
//     transfer->d.valid = true;
// }

std::optional<UsbDescriptorTree> getUSBDescriptorTree(const int fd) {
    USBIO::Transfer t;
    t.usb_fd = fd;
    t.type = USBIO::TransferType::CONTROL;

    // fixed size is dumb, but my camera as a total len of 206
    unsigned char buff[512];
    std::memset(buff, 0, sizeof(buff));
    transferForConfigDescriptor(&t, buff, 512);
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
                ep.wMaxPacketSize = static_cast<uint16_t>(buff[i + 4]) | (buff[i + 5] << 8);
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

namespace {
void IsoWorker(const int fd, const std::atomic_bool& keep_alive, IsochronousConfig cfg) {
    const uint16_t raw = cfg.ep.wMaxPacketSize;
    const uint16_t mps = raw & 0x07FF;
    const uint16_t mult  = ((raw >> 11) & 0x3) + 1; // high order is multiplier
    const uint16_t packet_size = mps * mult;

    const std::size_t packet_data_size =  packet_size * cfg.packets_per_urb;
    const std::size_t usbdevfs_urb_total_size =
        (sizeof(usbdevfs_iso_packet_desc) * cfg.packets_per_urb) + sizeof(usbdevfs_urb);
    const std::size_t buffer_ptr_array_size = sizeof(usbdevfs_urb*) * cfg.ring_size;

    usbdevfs_urb** buffer_ptr_array = static_cast<usbdevfs_urb**>(malloc(buffer_ptr_array_size));
    if (!buffer_ptr_array)
        throw std::runtime_error("null!");

    int r = 0;
    int init_buffs = 0;
    for (int i = 0; i < cfg.ring_size; ++i) {
        buffer_ptr_array[i] = static_cast<usbdevfs_urb*>(malloc(usbdevfs_urb_total_size));

        if (!buffer_ptr_array[i])
            throw std::runtime_error("null!");

        std::memset(buffer_ptr_array[i], 0, usbdevfs_urb_total_size);
        buffer_ptr_array[i]->type = USBDEVFS_URB_TYPE_ISO;
        buffer_ptr_array[i]->endpoint = cfg.ep.bEndpointAddress;
        buffer_ptr_array[i]->number_of_packets = cfg.packets_per_urb;
        buffer_ptr_array[i]->buffer = malloc(packet_data_size);
        buffer_ptr_array[i]->buffer_length = packet_data_size;

        init_buffs += 1;

        for (int p = 0; p < cfg.packets_per_urb; ++p) {
            buffer_ptr_array[i]->iso_frame_desc[p].length = packet_size;
            buffer_ptr_array[i]->iso_frame_desc[p].actual_length = 0;
            buffer_ptr_array[i]->iso_frame_desc[p].status = 0;
        }

        buffer_ptr_array[i]->flags = USBDEVFS_URB_ISO_ASAP;

        r = ioctl(fd, USBDEVFS_SUBMITURB, buffer_ptr_array[i]);
        if (r < 0) {
            std::cout << errno << std::endl;
            break;  // ERROR
        }
    }

    while (r >= 0 && keep_alive.load()) {
        usbdevfs_urb* completed = nullptr;
        r = ioctl(fd, USBDEVFS_REAPURB, &completed);
        if (!completed || r < 0)
            break;

        std::vector<IscPacketResults> packet_res;
        for (int p = 0; p < cfg.packets_per_urb; ++p)
            packet_res.push_back(IscPacketResults{
                .length = completed->iso_frame_desc[p].length,
                .actual_length = completed->iso_frame_desc[p].actual_length,
                .status = completed->iso_frame_desc[p].status});

        cfg.on_packet(
            static_cast<const std::uint8_t*>(completed->buffer),
            completed->buffer_length,
            std::move(packet_res));

        r = ioctl(fd, USBDEVFS_SUBMITURB, completed);
        if (r < 0)
            break;
    }

    if (r < 0)
        std::printf("ERROR %d %d\n", r, init_buffs);

    for (int i = 0; i < init_buffs; ++i) {
        free(buffer_ptr_array[i]->buffer);
        free(buffer_ptr_array[i]);
    }

    free(buffer_ptr_array);
}

}

InterfaceForIso::InterfaceForIso(const int fd, const InterfaceDescriptor& interface)
    : fd_(fd), interface_(interface)
{
    usbdevfs_ioctl cmd{
        .ifno = interface_.bInterfaceNumber,
        .ioctl_code = USBDEVFS_DISCONNECT,
        .data = nullptr
    };

    // disconnect kernel driver
    ioctl(fd_, USBDEVFS_IOCTL, &cmd);

    const int interface_num = interface_.bInterfaceNumber;
    const int r = ioctl(fd_, USBDEVFS_CLAIMINTERFACE, &interface_num);
    if (r < 0)
        throw std::runtime_error("Couldn't reserve interface");

    usbdevfs_setinterface setintf {
        .interface = interface_num,
        .altsetting = interface_.bAlternateSetting
    };

    int sr = ioctl(fd_, USBDEVFS_SETINTERFACE, &setintf);
    if (sr < 0)
      std::cout << "err SETINTERFACE iface=" << interface_num << "alt=" <<
            interface_.bAlternateSetting << "errno=" << strerror(errno) << std::endl;
}

InterfaceForIso::InterfaceForIso(InterfaceForIso&& other) noexcept
    : fd_(other.fd_), interface_(std::move(other.interface_)),
      ep_addr_to_thread_and_keep_alive_flag_(
          std::move(other.ep_addr_to_thread_and_keep_alive_flag_))
{ other.fd_ = -1; }

InterfaceForIso& InterfaceForIso::operator=(InterfaceForIso&& other) noexcept {
    if (this != &other) {
        fd_ = other.fd_;
        interface_ = std::move(other.interface_);
        ep_addr_to_thread_and_keep_alive_flag_ =
            std::move(other.ep_addr_to_thread_and_keep_alive_flag_);
        other.fd_ = -1;
    }
    return *this;
}

InterfaceForIso::~InterfaceForIso()
{
  if (fd_ < 0)
    return;

  for (const auto& running_trans : ep_addr_to_thread_and_keep_alive_flag_)
    stopIsochronousCapture(running_trans.first);

  const int interface_num = interface_.bInterfaceNumber;
  ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &interface_num);
}

TransferError InterfaceForIso::startIsochronousCapture(const IsochronousConfig& cfg)
{
    auto pair_it = ep_addr_to_thread_and_keep_alive_flag_.emplace(
        cfg.ep.bEndpointAddress,
        std::make_pair(
            std::thread{},
            std::make_unique<std::atomic_bool>(true)));

    if (!pair_it.second || pair_it.first == ep_addr_to_thread_and_keep_alive_flag_.cend())
      return TransferError::ERROR;

    const std::atomic_bool& keep_alive = *pair_it.first->second.second;
    pair_it.first->second.first =
        std::thread(IsoWorker, fd_, std::cref(keep_alive), cfg);
    return TransferError::SUCCESS;
}

// There is a deadlock if USBDEVFS_REAPURB hangs indefinitely, this will wait to join and
// USBDEVFS_REAPURB will hang (which is very possible).
// For now don't care as build up is more important than teardown.
TransferError InterfaceForIso::stopIsochronousCapture(uint8_t ep_address)
{
    const auto it = ep_addr_to_thread_and_keep_alive_flag_.find( ep_address);
    if (it == ep_addr_to_thread_and_keep_alive_flag_.cend())
        return TransferError::ERROR;

    it->second.second->store(false);
    if (it->second.first.joinable())
        it->second.first.join();

    return TransferError::SUCCESS;
}

}