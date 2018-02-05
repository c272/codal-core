/*
The MIT License (MIT)

Copyright (c) 2017 Lancaster University.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#include "CodalUSB.h"

#if CONFIG_ENABLED(DEVICE_USB)

#include "ErrorNo.h"
#include "CodalDmesg.h"
#include "codal_target_hal.h"

#define send(p, l) ctrlIn->write(p, l)

CodalUSB *CodalUSB::usbInstance = NULL;

//#define LOG DMESG
#define LOG(...)

static uint8_t usb_initialised = 0;
// usb_20.pdf
static uint8_t usb_status = 0;
// static uint8_t usb_suspended = 0; // copy of UDINT to check SUSPI and WAKEUPI bits
static uint8_t usb_configured = 0;

static const ConfigDescriptor static_config = {9, 2, 0, 0, 1, 0, USB_CONFIG_BUS_POWERED, 250};

static const DeviceDescriptor default_device_desc = {
    0x12,            // bLength
    0x01,            // bDescriptorType
    0x0200,          // bcdUSBL
    0xEF,            // bDeviceClass:    Misc
    0x02,            // bDeviceSubclass:
    0x01,            // bDeviceProtocol:
    0x40,            // bMaxPacketSize0
    USB_DEFAULT_VID, //
    USB_DEFAULT_PID, //
    0x4202,          // bcdDevice - leave unchanged for the HF2 to work
    0x01,            // iManufacturer
    0x02,            // iProduct
    0x03,            // SerialNumber
    0x01             // bNumConfigs
};

static const char *default_strings[] = {
    "CoDAL Devices",
    "Generic CoDAL device",
    "4242",
};

CodalUSB::CodalUSB()
{
    usbInstance = this;
    endpointsUsed = 1; // CTRL endpoint
    ctrlIn = NULL;
    ctrlOut = NULL;
    numStringDescriptors = sizeof(default_strings) / sizeof(default_strings[0]);
    stringDescriptors = default_strings;
    deviceDescriptor = &default_device_desc;
    startDelayCount = 1;
    interfaces = NULL;
}

void CodalUSBInterface::fillInterfaceInfo(InterfaceDescriptor *descp)
{
    const InterfaceInfo *info = this->getInterfaceInfo();
    InterfaceDescriptor desc = {
        sizeof(InterfaceDescriptor),
        4, // type
        this->interfaceIdx,
        info->iface.alternate,
        info->iface.numEndpoints,
        info->iface.interfaceClass,
        info->iface.interfaceSubClass,
        info->iface.protocol,
        info->iface.iInterfaceString,
    };
    *descp = desc;
}

int CodalUSB::sendConfig()
{
    const InterfaceInfo *info;
    int numInterfaces = 0;
    int clen = sizeof(ConfigDescriptor);

    // calculate the total size of our interfaces.
    for (CodalUSBInterface *iface = interfaces; iface; iface = iface->next)
    {
        info = iface->getInterfaceInfo();
        clen += sizeof(InterfaceDescriptor) +
                info->iface.numEndpoints * sizeof(EndpointDescriptor) +
                info->supplementalDescriptorSize;
        numInterfaces++;
    }

    uint8_t *buf = new uint8_t[clen];
    memcpy(buf, &static_config, sizeof(ConfigDescriptor));
    ((ConfigDescriptor *)buf)->clen = clen;
    ((ConfigDescriptor *)buf)->numInterfaces = numInterfaces;
    clen = sizeof(ConfigDescriptor);

#define ADD_DESC(desc)                                                                             \
    memcpy(buf + clen, &desc, sizeof(desc));                                                       \
    clen += sizeof(desc)

    // send our descriptors
    for (CodalUSBInterface *iface = interfaces; iface; iface = iface->next)
    {
        info = iface->getInterfaceInfo();
        InterfaceDescriptor desc;
        iface->fillInterfaceInfo(&desc);
        ADD_DESC(desc);

        if (info->supplementalDescriptorSize)
        {
            memcpy(buf + clen, info->supplementalDescriptor, info->supplementalDescriptorSize);
            clen += info->supplementalDescriptorSize;
        }

        EndpointDescriptor epdescIn = {
            sizeof(EndpointDescriptor),
            5, // type
            (uint8_t)(0x80 | iface->in->ep),
            info->epIn.attr,
            USB_MAX_PKT_SIZE,
            info->epIn.interval,
        };
        ADD_DESC(epdescIn);

        if (info->iface.numEndpoints == 1)
        {
            // OK
        }
        else if (info->iface.numEndpoints == 2)
        {
            EndpointDescriptor epdescOut = {
                sizeof(EndpointDescriptor),
                5, // type
                iface->out->ep,
                info->epIn.attr,
                USB_MAX_PKT_SIZE,
                info->epIn.interval,
            };
            ADD_DESC(epdescOut);
        }
        else
        {
            usb_assert(0);
        }
    }

    usb_assert(clen == ((ConfigDescriptor *)buf)->clen);

    send(buf, clen);

    delete buf;

    return DEVICE_OK;
}

// languageID - United States
static const uint8_t string0[] = {4, 3, 9, 4};

int CodalUSB::sendDescriptors(USBSetup &setup)
{
    uint8_t type = setup.wValueH;

    if (type == USB_CONFIGURATION_DESCRIPTOR_TYPE)
        return sendConfig();

    if (type == USB_DEVICE_DESCRIPTOR_TYPE)
        return send(deviceDescriptor, sizeof(DeviceDescriptor));

    else if (type == USB_STRING_DESCRIPTOR_TYPE)
    {
        // check if we exceed our bounds.
        if (setup.wValueL > numStringDescriptors)
            return DEVICE_NOT_SUPPORTED;

        if (setup.wValueL == 0)
            return send(string0, sizeof(string0));

        StringDescriptor desc;

        const char *str = stringDescriptors[setup.wValueL - 1];
        if (!str)
            return DEVICE_NOT_SUPPORTED;

        desc.type = 3;
        uint32_t len = strlen(str) * 2 + 2;
        desc.len = len;

        usb_assert(len <= sizeof(desc));

        int i = 0;
        while (*str)
            desc.data[i++] = *str++;

        // send the string descriptor the host asked for.
        return send(&desc, desc.len);
    }
    else
    {
        return interfaceRequest(setup, false);
    }

    return DEVICE_NOT_SUPPORTED;
}

CodalUSB *CodalUSB::getInstance()
{
    if (usbInstance == NULL)
        usbInstance = new CodalUSB;

    return usbInstance;
}

int CodalUSB::add(CodalUSBInterface &interface)
{
    usb_assert(!usb_configured);

    uint8_t epsConsumed = interface.getInterfaceInfo()->allocateEndpoints;

    if (endpointsUsed + epsConsumed > DEVICE_USB_ENDPOINTS)
        return DEVICE_NO_RESOURCES;

    interface.interfaceIdx = 0;

    CodalUSBInterface *iface;

    for (iface = interfaces; iface; iface = iface->next)
    {
        interface.interfaceIdx++;
        if (!iface->next)
            break;
    }

    if (iface)
        iface->next = &interface;
    else
        interfaces = &interface;
    interface.next = NULL;

    endpointsUsed += epsConsumed;

    return DEVICE_OK;
}

int CodalUSB::isInitialised()
{
    return usb_initialised > 0;
}

int CodalUSB::interfaceRequest(USBSetup &setup, bool isClass)
{
    int ifaceIdx = -1;
    int epIdx = -1;

    if ((setup.bmRequestType & USB_REQ_DESTINATION) == USB_REQ_INTERFACE)
        ifaceIdx = setup.wIndex & 0xff;
    else if ((setup.bmRequestType & USB_REQ_DESTINATION) == USB_REQ_ENDPOINT)
        epIdx = setup.wIndex & 0x7f;

    LOG("iface req: ifaceIdx=%d epIdx=%d", ifaceIdx, epIdx);

    for (CodalUSBInterface *iface = interfaces; iface; iface = iface->next)
    {
        if (iface->interfaceIdx == ifaceIdx ||
            ((iface->in && iface->in->ep == epIdx) || (iface->out && iface->out->ep == epIdx)))
        {
            int res =
                isClass ? iface->classRequest(*ctrlIn, setup) : iface->stdRequest(*ctrlIn, setup);
            LOG("iface req res=%d", res);
            if (res == DEVICE_OK)
                return DEVICE_OK;
        }
    }

    return DEVICE_NOT_SUPPORTED;
}

#define sendzlp() send(&usb_status, 0)
#define stall ctrlIn->stall

void CodalUSB::setupRequest(USBSetup &setup)
{
    DMESG("SETUP Req=%x type=%x val=%x:%x idx=%x len=%d", setup.bRequest, setup.bmRequestType,
          setup.wValueH, setup.wValueL, setup.wIndex, setup.wLength);

    int status = DEVICE_OK;

    // Standard Requests
    uint16_t wValue = (setup.wValueH << 8) | setup.wValueL;
    uint8_t request_type = setup.bmRequestType;
    uint16_t wStatus = 0;

    ctrlIn->wLength = setup.wLength;

    if ((request_type & USB_REQ_TYPE) == USB_REQ_STANDARD)
    {
        switch (setup.bRequest)
        {
        case USB_REQ_GET_STATUS:
            if (request_type == (USB_REQ_DEVICETOHOST | USB_REQ_STANDARD | USB_REQ_DEVICE))
            {
                wStatus = usb_status;
            }
            send(&wStatus, sizeof(wStatus));
            break;

        case USB_REQ_CLEAR_FEATURE:
            if ((request_type == (USB_REQ_HOSTTODEVICE | USB_REQ_STANDARD | USB_REQ_DEVICE)) &&
                (wValue == USB_DEVICE_REMOTE_WAKEUP))
                usb_status &= ~USB_FEATURE_REMOTE_WAKEUP_ENABLED;

            if (request_type == (USB_REQ_HOSTTODEVICE | USB_REQ_STANDARD | USB_REQ_ENDPOINT))
            {
                for (CodalUSBInterface *iface = interfaces; iface; iface = iface->next)
                {
                    if (iface->in && iface->in->ep == (setup.wIndex & 0x7f))
                        iface->in->clearStall();
                    else if (iface->out && iface->out->ep == (setup.wIndex & 0x7f))
                        iface->out->clearStall();
                }
            }
            sendzlp();
            break;
        case USB_REQ_SET_FEATURE:
            if ((request_type == (USB_REQ_HOSTTODEVICE | USB_REQ_STANDARD | USB_REQ_DEVICE)) &&
                (wValue == USB_DEVICE_REMOTE_WAKEUP))
                usb_status |= USB_FEATURE_REMOTE_WAKEUP_ENABLED;
            sendzlp();
            break;
        case USB_REQ_SET_ADDRESS:
            sendzlp();
            usb_set_address(wValue);
            break;
        case USB_REQ_GET_DESCRIPTOR:
            status = sendDescriptors(setup);
            break;
        case USB_REQ_SET_DESCRIPTOR:
            stall();
            break;
        case USB_REQ_GET_CONFIGURATION:
            wStatus = 1;
            send(&wStatus, 1);
            break;

        case USB_REQ_SET_CONFIGURATION:
            if (USB_REQ_DEVICE == (request_type & USB_REQ_DESTINATION))
            {
                usb_initialised = setup.wValueL;
                sendzlp();
            }
            else
                status = DEVICE_NOT_SUPPORTED;
            break;
        }
    }
    else
    {
        status = interfaceRequest(setup, true);
    }

    if (status < 0)
        stall();

    // sending response clears this - make sure we did
    usb_assert(ctrlIn->wLength == 0);
}

void CodalUSB::interruptHandler()
{
    for (CodalUSBInterface *iface = interfaces; iface; iface = iface->next)
        iface->endpointRequest();
}

void CodalUSB::initEndpoints()
{
    uint8_t endpointCount = 1;

    if (ctrlIn)
    {
        delete ctrlIn;
        delete ctrlOut;
    }

    ctrlIn = new UsbEndpointIn(0, USB_EP_TYPE_CONTROL);
    ctrlOut = new UsbEndpointOut(0, USB_EP_TYPE_CONTROL);

    for (CodalUSBInterface *iface = interfaces; iface; iface = iface->next)
    {
        const InterfaceInfo *info = iface->getInterfaceInfo();

        usb_assert(1 <= info->allocateEndpoints && info->allocateEndpoints <= 2);
        usb_assert(info->allocateEndpoints <= info->iface.numEndpoints &&
                   info->iface.numEndpoints <= 2);

        if (iface->in)
            delete iface->in;
        if (iface->out)
        {
            delete iface->out;
            iface->out = NULL;
        }

        iface->in = new UsbEndpointIn(endpointCount, info->epIn.attr);
        if (info->iface.numEndpoints > 1)
        {
            iface->out =
                new UsbEndpointOut(endpointCount + (info->allocateEndpoints - 1), info->epIn.attr);
        }

        endpointCount += info->allocateEndpoints;
    }

    usb_assert(endpointsUsed == endpointCount);
}

int CodalUSB::start()
{
    if (--startDelayCount > 0)
    {
        DMESG("USB start delayed");
        return DEVICE_OK;
    }

    DMESG("USB start");

    if (DEVICE_USB_ENDPOINTS == 0)
        return DEVICE_NOT_SUPPORTED;

    if (usb_configured)
        return DEVICE_OK;

    usb_configured = 1;

    usb_configure(endpointsUsed);

    return DEVICE_OK;
}

void usb_panic(int lineNumber)
{
    DMESG("USB assertion failed: line %d", lineNumber);
    target_panic(DEVICE_USB_ERROR);
}

#endif
