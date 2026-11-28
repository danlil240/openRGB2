// SPDX-License-Identifier: GPL-2.0-or-later
//
// fake_libusb -- see fake_libusb.h for the model and the API.

#include "fake_libusb.h"

#include <algorithm>
#include <cstring>

/* Pinned to the vendored header: a libusb bump must revisit the
   stubbed prototypes below (audit P-13 risk note). */
static_assert(LIBUSB_API_VERSION == 0x0100010A,
              "fake_libusb is written against vendored libusb 1.0.27");

/* The opaque libusb types get test-side bodies. */
struct libusb_device
{
    int index;
};

struct libusb_device_handle
{
    int index;
};

namespace fake_libusb
{
namespace
{
    std::vector<Device>                   g_devices;
    std::deque<libusb_device>             g_device_objs;  /* stable addresses */
    Globals                               g_globals;
    std::vector<libusb_device_handle*>    g_handles;      /* for Reset() */

    libusb_device* DeviceObj(int index)
    {
        return &g_device_objs[(size_t)index];
    }

    bool Valid(int index)
    {
        return index >= 0 && (size_t)index < g_devices.size();
    }
}

std::vector<Transfer> Device::Reads() const
{
    std::vector<Transfer> out;
    for(const Transfer& t : log)
    {
        if(t.in) out.push_back(t);
    }
    return out;
}

std::vector<Transfer> Device::Writes() const
{
    std::vector<Transfer> out;
    for(const Transfer& t : log)
    {
        if(!t.in) out.push_back(t);
    }
    return out;
}

void Reset()
{
    /* Detach handles left over from an earlier case: they now
       point at index -1 and every call on them fails NO_DEVICE. */
    for(libusb_device_handle* h : g_handles)
    {
        h->index = -1;
    }
    g_handles.clear();
    g_devices.clear();
    g_device_objs.clear();
    g_globals = Globals{};
}

int AddDevice(uint16_t vid, uint16_t pid)
{
    Device d;
    d.vid = vid;
    d.pid = pid;
    g_devices.push_back(d);
    const int index = (int)g_devices.size() - 1;
    g_device_objs.push_back(libusb_device{ index });
    return index;
}

Device& Dev(int index)
{
    return g_devices.at((size_t)index);
}

Globals& G()
{
    return g_globals;
}

void QueueRead(int dev, std::vector<uint8_t> data, int rc)
{
    ReadChunk c;
    c.data = std::move(data);
    c.rc   = rc;
    c.after_writes = 0;
    Dev(dev).reads_queue.push_back(std::move(c));
}

void QueueReply(int dev, std::vector<uint8_t> data, int rc)
{
    ReadChunk c;
    c.data = std::move(data);
    c.rc   = rc;
    c.after_writes = Dev(dev).writes + 1;
    Dev(dev).reads_queue.push_back(std::move(c));
}

void QueueReadError(int dev, int rc, bool after_next_write)
{
    ReadChunk c;
    c.rc = rc;
    c.after_writes = after_next_write ? Dev(dev).writes + 1 : 0;
    Dev(dev).reads_queue.push_back(std::move(c));
}

void QueueWrite(int dev, int rc, int transferred)
{
    WriteResult w;
    w.rc          = rc;
    w.transferred = transferred;
    Dev(dev).writes_queue.push_back(w);
}

void Unplug(int dev)
{
    Dev(dev).present = false;
}

void Replug(int dev)
{
    Dev(dev).present = true;
}

} /* namespace fake_libusb */

using namespace fake_libusb;

/*---------------------------------------------------------*\
| The stubbed C API. Defined inside extern "C" so a          |
| prototype that drifts from the vendored header is a        |
| compile error (C linkage forbids the silent overload).     |
\*---------------------------------------------------------*/
extern "C"
{

int LIBUSB_CALL libusb_init(libusb_context** ctx)
{
    if(ctx != nullptr)
    {
        *ctx = nullptr;
    }
    ++g_globals.init_calls;
    return g_globals.init_rc;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context* ctx,
                                           libusb_device*** list)
{
    (void)ctx;
    ++g_globals.list_calls;
    if(g_globals.list_rc < 0)
    {
        return g_globals.list_rc;
    }
    std::vector<libusb_device*> present;
    for(size_t i = 0; i < g_devices.size(); ++i)
    {
        if(g_devices[i].present)
        {
            present.push_back(DeviceObj((int)i));
        }
    }
    libusb_device** arr = new libusb_device*[present.size() + 1];
    for(size_t i = 0; i < present.size(); ++i)
    {
        arr[i] = present[i];
    }
    arr[present.size()] = nullptr;
    *list = arr;
    ++g_globals.lists_live;
    return (ssize_t)present.size();
}

void LIBUSB_CALL libusb_free_device_list(libusb_device** list,
                                         int unref_devices)
{
    (void)unref_devices;
    if(list == nullptr)
    {
        return;
    }
    delete[] list;
    --g_globals.lists_live;
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device* dev,
    struct libusb_device_descriptor* desc)
{
    if(dev == nullptr || !Valid(dev->index))
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    const Device& d = g_devices[(size_t)dev->index];
    if(d.descriptor_rc != 0)
    {
        return d.descriptor_rc;
    }
    std::memset(desc, 0, sizeof(*desc));
    desc->bLength         = LIBUSB_DT_DEVICE_SIZE;
    desc->bDescriptorType = LIBUSB_DT_DEVICE;
    desc->idVendor        = d.vid;
    desc->idProduct       = d.pid;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_open(libusb_device* dev,
                            libusb_device_handle** dev_handle)
{
    if(dev == nullptr || !Valid(dev->index))
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    Device& d = g_devices[(size_t)dev->index];
    if(!d.present)
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    if(d.open_rc != 0)
    {
        return d.open_rc;
    }
    libusb_device_handle* h = new libusb_device_handle{ dev->index };
    g_handles.push_back(h);
    ++d.opens;
    ++d.live_handles;
    *dev_handle = h;
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_close(libusb_device_handle* dev_handle)
{
    if(dev_handle == nullptr)
    {
        return;
    }
    if(Valid(dev_handle->index))
    {
        Device& d = g_devices[(size_t)dev_handle->index];
        ++d.closes;
        --d.live_handles;
    }
    g_handles.erase(std::remove(g_handles.begin(), g_handles.end(),
                                dev_handle),
                    g_handles.end());
    delete dev_handle;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle* dev_handle,
                                       int interface_number)
{
    (void)interface_number;
    if(dev_handle == nullptr || !Valid(dev_handle->index))
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    Device& d = g_devices[(size_t)dev_handle->index];
    if(d.claim_rc != 0)
    {
        return d.claim_rc;
    }
    ++d.claims;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle* dev_handle,
                                         int interface_number)
{
    (void)interface_number;
    if(dev_handle == nullptr || !Valid(dev_handle->index))
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    ++g_devices[(size_t)dev_handle->index].releases;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(
    libusb_device_handle* dev_handle, int enable)
{
    (void)enable;
    if(dev_handle == nullptr || !Valid(dev_handle->index))
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    ++g_devices[(size_t)dev_handle->index].auto_detach_calls;
    /* WinUSB: unsupported, which the transport ignores. */
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle* dev_handle,
    unsigned char endpoint, unsigned char* data, int length,
    int* actual_length, unsigned int timeout)
{
    int moved = 0;
    if(actual_length != nullptr)
    {
        *actual_length = 0;
    }
    if(dev_handle == nullptr || !Valid(dev_handle->index))
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    const int index = dev_handle->index;
    Device&   d     = g_devices[(size_t)index];

    Transfer t;
    t.in         = (endpoint & LIBUSB_ENDPOINT_IN) != 0;
    t.endpoint   = endpoint;
    t.length     = length;
    t.timeout_ms = timeout;

    if(!d.present)
    {
        t.rc = LIBUSB_ERROR_NO_DEVICE;
        if(t.in) ++d.reads; else ++d.writes;
    }
    else if(t.in)
    {
        ++d.reads;
        if(d.reads_queue.empty() ||
           d.reads_queue.front().after_writes > d.writes)
        {
            t.rc = LIBUSB_ERROR_TIMEOUT;       /* silence */
        }
        else
        {
            ReadChunk c = std::move(d.reads_queue.front());
            d.reads_queue.pop_front();
            if(c.rc != LIBUSB_SUCCESS && c.rc != LIBUSB_ERROR_TIMEOUT)
            {
                t.rc = c.rc;
            }
            else
            {
                moved = (int)c.data.size();
                t.rc  = c.rc;
                if(moved > length)
                {
                    moved = length;
                    t.rc  = LIBUSB_ERROR_OVERFLOW;
                }
                if(moved > 0)
                {
                    std::memcpy(data, c.data.data(), (size_t)moved);
                }
                t.data.assign(c.data.begin(), c.data.begin() + moved);
            }
        }
    }
    else
    {
        ++d.writes;
        WriteResult w;
        if(!d.writes_queue.empty())
        {
            w = d.writes_queue.front();
            d.writes_queue.pop_front();
        }
        t.rc = w.rc;
        if(w.rc == LIBUSB_SUCCESS || w.rc == LIBUSB_ERROR_TIMEOUT)
        {
            moved = (w.transferred < 0) ? length
                                        : std::min(w.transferred, length);
            t.data.assign(data, data + moved);
        }
    }

    if(actual_length != nullptr)
    {
        *actual_length = moved;
    }
    d.log.push_back(t);
    if(g_globals.on_transfer)
    {
        g_globals.on_transfer(index, d.log.back());
    }
    return t.rc;
}

} /* extern "C" */
