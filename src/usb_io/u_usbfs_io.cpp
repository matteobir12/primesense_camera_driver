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

InterfaceForIso::InterfaceForIso(const int fd, const InterfaceDescriptor& interface)
    : fd_(fd), interface_(interface),
      keep_alive_(std::make_unique<std::atomic_bool>(true)),
      captures_mutex_(std::make_unique<std::mutex>())
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
      reaper_(std::move(other.reaper_)),
      keep_alive_(std::move(other.keep_alive_)),
      captures_mutex_(std::move(other.captures_mutex_)),
      captures_(std::move(other.captures_)),
      bulk_readers_(std::move(other.bulk_readers_))
{ other.fd_ = -1; }

InterfaceForIso& InterfaceForIso::operator=(InterfaceForIso&& other) noexcept {
    if (this != &other) {
        fd_ = other.fd_;
        interface_ = std::move(other.interface_);
        reaper_ = std::move(other.reaper_);
        keep_alive_ = std::move(other.keep_alive_);
        captures_mutex_ = std::move(other.captures_mutex_);
        captures_ = std::move(other.captures_);
        bulk_readers_ = std::move(other.bulk_readers_);
        other.fd_ = -1;
    }
    return *this;
}

void InterfaceForIso::freeCapture(EpCapture& capture)
{
    for (usbdevfs_urb* const urb : capture.urbs) {
        ioctl(fd_, USBDEVFS_DISCARDURB, urb);
        free(urb->buffer);
        free(urb);
    }
    capture.urbs.clear();
}

InterfaceForIso::~InterfaceForIso()
{
  if (fd_ < 0)
    return;

  if (keep_alive_)
      keep_alive_->store(false);

  // There is a deadlock if USBDEVFS_REAPURB hangs indefinitely; discarding the
  // URBs makes them complete (with error status) so the reaper wakes up.
  {
      std::lock_guard<std::mutex> lock(*captures_mutex_);
      for (auto& [ep, capture] : captures_)
          for (usbdevfs_urb* const urb : capture.urbs)
              ioctl(fd_, USBDEVFS_DISCARDURB, urb);
  }

  if (reaper_.joinable())
      reaper_.join();

  // bulk readers exit on their own within the read timeout
  for (auto& reader : bulk_readers_)
      if (reader.joinable())
          reader.join();

  for (auto& [ep, capture] : captures_)
      freeCapture(capture);

  const int interface_num = interface_.bInterfaceNumber;
  ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &interface_num);
}

TransferError InterfaceForIso::startBulkCapture(const BulkCaptureConfig& cfg)
{
    if (!cfg.on_data || cfg.chunk_bytes <= 0)
        return TransferError::ERROR;

    bulk_readers_.emplace_back(
        [fd = fd_, keep_alive = keep_alive_.get(), cfg]() {
            std::vector<std::uint8_t> buf(cfg.chunk_bytes);

            while (keep_alive->load()) {
                usbdevfs_bulktransfer bt;
                std::memset(&bt, 0, sizeof(bt));
                bt.ep = cfg.ep.bEndpointAddress;
                bt.len = buf.size();
                bt.timeout = 1000;  // ms; loop to check keep_alive
                bt.data = buf.data();

                const int r = ioctl(fd, USBDEVFS_BULK, &bt);
                if (r > 0)
                    cfg.on_data(buf.data(), r);
                else if (r < 0 && errno != ETIMEDOUT && errno != EAGAIN && errno != EINTR)
                    break;  // device gone
            }
        });

    return TransferError::SUCCESS;
}

TransferError InterfaceForIso::startIsochronousCapture(const IsochronousConfig& cfg)
{
    const uint16_t raw = cfg.ep.wMaxPacketSize;
    const uint16_t mps = raw & 0x07FF;
    const uint16_t mult = ((raw >> 11) & 0x3) + 1; // high order is multiplier
    const uint16_t packet_size = mps * mult;

    const std::size_t packet_data_size = packet_size * cfg.packets_per_urb;
    const std::size_t usbdevfs_urb_total_size =
        (sizeof(usbdevfs_iso_packet_desc) * cfg.packets_per_urb) + sizeof(usbdevfs_urb);

    EpCapture capture;
    capture.cfg = cfg;

    for (int i = 0; i < cfg.ring_size; ++i) {
        usbdevfs_urb* const urb = static_cast<usbdevfs_urb*>(malloc(usbdevfs_urb_total_size));
        if (!urb)
            throw std::runtime_error("null!");

        std::memset(urb, 0, usbdevfs_urb_total_size);
        urb->type = USBDEVFS_URB_TYPE_ISO;
        urb->endpoint = cfg.ep.bEndpointAddress;
        urb->number_of_packets = cfg.packets_per_urb;
        urb->buffer = malloc(packet_data_size);
        urb->buffer_length = packet_data_size;
        urb->flags = USBDEVFS_URB_ISO_ASAP;

        for (int p = 0; p < cfg.packets_per_urb; ++p)
            urb->iso_frame_desc[p].length = packet_size;

        capture.urbs.push_back(urb);

        if (ioctl(fd_, USBDEVFS_SUBMITURB, urb) < 0) {
            std::cout << "SUBMITURB failed: " << strerror(errno) << std::endl;
            freeCapture(capture);
            return TransferError::ERROR;
        }
    }

    {
        std::lock_guard<std::mutex> lock(*captures_mutex_);
        if (captures_.count(cfg.ep.bEndpointAddress)) {
            freeCapture(capture);
            return TransferError::ERROR;  // already capturing on this ep
        }

        captures_.emplace(cfg.ep.bEndpointAddress, std::move(capture));
    }

    if (!reaper_.joinable())
        reaper_ = std::thread(&InterfaceForIso::reaperLoop, this);

    return TransferError::SUCCESS;
}

void InterfaceForIso::reaperLoop()
{
    while (keep_alive_->load()) {
        usbdevfs_urb* completed = nullptr;
        const int r = ioctl(fd_, USBDEVFS_REAPURB, &completed);
        if (r < 0 || !completed) {
            if (errno == EINTR)
                continue;
            break;  // device gone or fd closed
        }

        // route by endpoint; REAPURB hands back completions for the whole fd
        IsochronousConfig* cfg = nullptr;
        {
            std::lock_guard<std::mutex> lock(*captures_mutex_);
            const auto it = captures_.find(completed->endpoint);
            if (it != captures_.end())
                cfg = &it->second.cfg;
        }

        if (!cfg)
            continue;  // capture was stopped, urb already freed/discarded

        std::vector<IscPacketResults> packet_res;
        packet_res.reserve(completed->number_of_packets);
        for (int p = 0; p < completed->number_of_packets; ++p)
            packet_res.push_back(IscPacketResults{
                .length = completed->iso_frame_desc[p].length,
                .actual_length = completed->iso_frame_desc[p].actual_length,
                .status = completed->iso_frame_desc[p].status});

        cfg->on_packet(
            static_cast<const std::uint8_t*>(completed->buffer),
            completed->buffer_length,
            std::move(packet_res));

        if (ioctl(fd_, USBDEVFS_SUBMITURB, completed) < 0)
            std::cout << "iso resubmit failed: " << strerror(errno) << std::endl;
    }
}

TransferError InterfaceForIso::stopIsochronousCapture(uint8_t ep_address)
{
    std::lock_guard<std::mutex> lock(*captures_mutex_);
    const auto it = captures_.find(ep_address);
    if (it == captures_.cend())
        return TransferError::ERROR;

    freeCapture(it->second);
    captures_.erase(it);

    return TransferError::SUCCESS;
}

}