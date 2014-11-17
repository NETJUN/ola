/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * EuroliteProWidget.cpp
 * The synchronous and asynchronous EurolitePro widgets.
 * Copyright (C) 2014 Simon Newton
 */

#include "plugins/usbdmx/EuroliteProWidget.h"

#include <string.h>
#include <string>

#include "ola/Constants.h"
#include "ola/Logging.h"
#include "plugins/usbdmx/AsyncUsbSender.h"
#include "plugins/usbdmx/LibUsbAdaptor.h"
#include "plugins/usbdmx/ThreadedUsbSender.h"

namespace ola {
namespace plugin {
namespace usbdmx {

using std::string;

namespace {

// Why is this so long?
static const unsigned int URB_TIMEOUT_MS = 500;
static const uint8_t DMX_LABEL = 6;
static const unsigned char ENDPOINT = 0x02;
enum { EUROLITE_PRO_FRAME_SIZE = 518 };

/*
 * Create a Eurolite Pro message to match the supplied DmxBuffer.
 */
void CreateFrame(
    const DmxBuffer &buffer,
    uint8_t frame[EUROLITE_PRO_FRAME_SIZE]) {
  unsigned int frame_size = buffer.Size();

  // header
  frame[0] = 0x7E;   // Start message delimiter
  frame[1] = DMX_LABEL;      // Label
  frame[4] = DMX512_START_CODE;
  buffer.Get(frame + 5, &frame_size);
  frame[2] = (DMX_UNIVERSE_SIZE + 1) & 0xff;  // Data length LSB.
  frame[3] = ((DMX_UNIVERSE_SIZE + 1) >> 8);  // Data length MSB
  memset(frame + 5 + frame_size, 0, DMX_UNIVERSE_SIZE - frame_size);
  // End message delimiter

  frame[EUROLITE_PRO_FRAME_SIZE - 1] =  0xE7;
}

/**
 * Find the interface with the endpoint we're after. Usually this is interface
 * 1 but we check them all just in case.
 */
bool LocateInterface(libusb_device *usb_device,
                     int *interface_number) {
  struct libusb_config_descriptor *device_config;
  if (libusb_get_config_descriptor(usb_device, 0, &device_config) != 0) {
    OLA_WARN << "Failed to get device config descriptor";
    return false;
  }

  OLA_DEBUG << static_cast<int>(device_config->bNumInterfaces) <<
    " interfaces found";
  for (unsigned int i = 0; i < device_config->bNumInterfaces; i++) {
    const struct libusb_interface *interface = &device_config->interface[i];
    for (int j = 0; j < interface->num_altsetting; j++) {
      const struct libusb_interface_descriptor *iface_descriptor =
        &interface->altsetting[j];
      for (uint8_t k = 0; k < iface_descriptor->bNumEndpoints; k++) {
        const struct libusb_endpoint_descriptor *endpoint =
          &iface_descriptor->endpoint[k];
        OLA_DEBUG << "Interface " << i << ", altsetting " << j << ", endpoint "
          << static_cast<int>(k) << ", endpoint address 0x" << std::hex <<
          static_cast<int>(endpoint->bEndpointAddress);
        if (endpoint->bEndpointAddress == ENDPOINT) {
          OLA_INFO << "Using interface " << i;
          *interface_number = i;
          libusb_free_config_descriptor(device_config);
          return true;
        }
      }
    }
  }
  OLA_WARN << "Failed to locate endpoint for EurolitePro device.";
  libusb_free_config_descriptor(device_config);
  return false;
}
}  // namespace

// EuroliteProThreadedSender
// -----------------------------------------------------------------------------

/*
 * Sends messages to a EurolitePro device in a separate thread.
 */
class EuroliteProThreadedSender: public ThreadedUsbSender {
 public:
  EuroliteProThreadedSender(libusb_device *usb_device,
                            libusb_device_handle *handle);

 private:
  bool TransmitBuffer(libusb_device_handle *handle,
                      const DmxBuffer &buffer);
};

EuroliteProThreadedSender::EuroliteProThreadedSender(
    libusb_device *usb_device,
    libusb_device_handle *usb_handle)
    : ThreadedUsbSender(usb_device, usb_handle) {
}

bool EuroliteProThreadedSender::TransmitBuffer(libusb_device_handle *handle,
                                               const DmxBuffer &buffer) {
  uint8_t frame[EUROLITE_PRO_FRAME_SIZE];
  CreateFrame(buffer, frame);

  int transferred;
  int r = libusb_bulk_transfer(
      handle,
      ENDPOINT,
      frame,
      EUROLITE_PRO_FRAME_SIZE,
      &transferred,
      URB_TIMEOUT_MS);
  if (transferred != EUROLITE_PRO_FRAME_SIZE) {
    // not sure if this is fatal or not
    OLA_WARN << "EurolitePro driver failed to transfer all data";
  }
  return r == 0;
}

// SynchronousEuroliteProWidget
// -----------------------------------------------------------------------------

SynchronousEuroliteProWidget::SynchronousEuroliteProWidget(
    LibUsbAdaptor *adaptor,
    libusb_device *usb_device,
    const string &serial)
    : EuroliteProWidget(adaptor, serial),
      m_usb_device(usb_device) {
}

bool SynchronousEuroliteProWidget::Init() {
  libusb_device_handle *usb_handle;

  int interface_number;
  if (!LocateInterface(m_usb_device, &interface_number)) {
    return false;
  }

  bool ok = m_adaptor->OpenDeviceAndClaimInterface(
      m_usb_device, interface_number, &usb_handle);
  if (!ok) {
    return false;
  }

  std::auto_ptr<EuroliteProThreadedSender> sender(
      new EuroliteProThreadedSender(m_usb_device, usb_handle));
  if (!sender->Start()) {
    return false;
  }
  m_sender.reset(sender.release());
  return true;
}

bool SynchronousEuroliteProWidget::SendDMX(const DmxBuffer &buffer) {
  return m_sender.get() ? m_sender->SendDMX(buffer) : false;
}

// EuroliteProAsyncUsbSender
// -----------------------------------------------------------------------------
class EuroliteProAsyncUsbSender : public AsyncUsbSender {
 public:
  EuroliteProAsyncUsbSender(LibUsbAdaptor *adaptor,
                            libusb_device *usb_device)
      : AsyncUsbSender(adaptor, usb_device) {
  }

  ~EuroliteProAsyncUsbSender() {
    CancelTransfer();
  }

  libusb_device_handle* SetupHandle() {
    int interface_number;
    if (!LocateInterface(m_usb_device, &interface_number)) {
      return NULL;
    }

    libusb_device_handle *usb_handle;
    bool ok = m_adaptor->OpenDeviceAndClaimInterface(
        m_usb_device, 0, &usb_handle);
    return ok ? usb_handle : NULL;
  }

  void PerformTransfer(const DmxBuffer &buffer) {
    CreateFrame(buffer, m_tx_frame);
    FillBulkTransfer(ENDPOINT, m_tx_frame, EUROLITE_PRO_FRAME_SIZE,
                     URB_TIMEOUT_MS);
    SubmitTransfer();
  }

 private:
  uint8_t m_tx_frame[EUROLITE_PRO_FRAME_SIZE];

  DISALLOW_COPY_AND_ASSIGN(EuroliteProAsyncUsbSender);
};

// AsynchronousEuroliteProWidget
// -----------------------------------------------------------------------------

AsynchronousEuroliteProWidget::AsynchronousEuroliteProWidget(
    LibUsbAdaptor *adaptor,
    libusb_device *usb_device,
    const string &serial)
    : EuroliteProWidget(adaptor, serial) {
  m_sender.reset(new EuroliteProAsyncUsbSender(m_adaptor, usb_device));
}

bool AsynchronousEuroliteProWidget::Init() {
  return m_sender->Init();
}

bool AsynchronousEuroliteProWidget::SendDMX(const DmxBuffer &buffer) {
  return m_sender->SendDMX(buffer);
}
}  // namespace usbdmx
}  // namespace plugin
}  // namespace ola
