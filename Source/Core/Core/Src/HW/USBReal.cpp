// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "../Core.h"
#include "USBReal.h"
#include "CoreTiming.h"

#if defined(_MSC_VER) || defined(__MINGW32__)
#  include <time.h>
#ifndef _TIMEVAL_DEFINED /* also in winsock[2].h */
#define _TIMEVAL_DEFINED
struct timeval {
	long tv_sec;
	long tv_usec;
};
#endif /* _TIMEVAL_DEFINED */
#else
#  include <sys/time.h>
#endif

template <typename T>
static inline T& EmplaceBack(std::vector<T>& Vec)
{
	T Result;
	Vec.push_back(Result);
	return Vec.back();
}

namespace USBInterface
{

class CUSBRequestReal : public CUSBRequest
{
public:
	CUSBRequestReal(IUSBDevice* Device, void* UserData, s16 Endpoint, libusb_transfer* Transfer, void* UserPayload = NULL)
	: CUSBRequest(Device, UserData, Endpoint), m_Transfer(Transfer), m_UserPayload(UserPayload) {}
	libusb_transfer* m_Transfer;
	void* m_UserPayload;
};

CUSBDeviceReal::CUSBDeviceReal(libusb_device* Device, u32 Uid, libusb_device_handle* Handle, CUSBControllerReal* Controller, IUSBDeviceClient* Client)
: IUSBDevice(Client),
m_Device(Device), m_DeviceHandle(Handle), m_Controller(Controller)
{
	m_NumInterfaces = 0;

	libusb_device_descriptor Desc;
	int Err = libusb_get_device_descriptor(Device, &Desc);
	if (Err)
	{
		WARN_LOG(USBINTERFACE, "libusb_get_device_descriptor failed with error: %d", Err);
		m_Vid = m_Pid = 0xffff;
	}
	else
	{
		m_Vid = Desc.idVendor;
		m_Pid = Desc.idProduct;
	}
	m_Uid = Uid;
}

void CUSBDeviceReal::_Close()
{
	m_Controller->m_OpenedDevices.erase(m_Device);
	for (int i = 0; i < m_NumInterfaces; i++)
	{
		libusb_release_interface(m_DeviceHandle, i);
		libusb_attach_kernel_driver(m_DeviceHandle, i);
	}
	libusb_close(m_DeviceHandle);
}

u32 CUSBDeviceReal::SetConfig(int Config)
{
	DEBUG_LOG(USBINTERFACE, "USBReal: set config %d", Config);
	for (int i = 0; i < m_NumInterfaces; i++)
	{
		libusb_release_interface(m_DeviceHandle, i);
	}
	int Ret = libusb_set_configuration(m_DeviceHandle, Config);
	if (Ret)
	{
		WARN_LOG(USBINTERFACE, "libusb_set_configuration failed with error: %d", Ret);
		return -1000;
	}
	struct libusb_config_descriptor *ConfigDesc = NULL;
	Ret = libusb_get_config_descriptor(m_Device, Config, &ConfigDesc);
	if (Ret)
	{
		WARN_LOG(USBINTERFACE, "libusb_get_config_descriptor failed with error: %d", Ret);
		return -1000;
	}
	m_NumInterfaces = ConfigDesc->bNumInterfaces;
	libusb_free_config_descriptor(ConfigDesc);
	for (int i = 0; i < m_NumInterfaces; i++)
	{
		Ret = libusb_kernel_driver_active(m_DeviceHandle, i);
		DEBUG_LOG(USBINTERFACE, "kernel driver active[%d]: %d", i, Ret);
		if (Ret == 1)
		{
			Ret = libusb_detach_kernel_driver(m_DeviceHandle, i);
			if (Ret && Ret != LIBUSB_ERROR_NOT_SUPPORTED)
			{
				WARN_LOG(USBINTERFACE, "libusb_detach_kernel_driver failed with error: %d", Ret);
			}
		}
		else if (Ret && Ret != LIBUSB_ERROR_NOT_SUPPORTED)
		{
			WARN_LOG(USBINTERFACE, "libusb_kernel_driver_active error ret = %d", Ret);
		}

		Ret = libusb_claim_interface(m_DeviceHandle, i);
		if (Ret)
		{
			WARN_LOG(USBINTERFACE, "libusb_claim_interface(%d) failed with error: %d", i, Ret);
			continue;
		}
	}
	return 0;
}

u32 CUSBDeviceReal::SetInterfaceAltSetting(int Interface, int Setting)
{
	DEBUG_LOG(USBINTERFACE, "USBReal: set %d alt setting %d", Interface, Setting);
	int Ret = libusb_set_interface_alt_setting(m_DeviceHandle, Interface, Setting);
	if (Ret)
	{
		WARN_LOG(USBINTERFACE, "libusb_set_interface_alt_setting failed with error: %d", Ret);
		return -1000;
	}
	return 0;
}

void CUSBDeviceReal::CancelRequest(CUSBRequest* Request)
{
	libusb_cancel_transfer(((CUSBRequestReal*) Request)->m_Transfer);
}

void CUSBDeviceReal::TransferCallback(libusb_transfer* Transfer)
{
	CUSBRequestReal* Request = (CUSBRequestReal*) Transfer->user_data;
	DEBUG_LOG(USBINTERFACE, "USBReal: transfer callback Request=%p status=%d", Request, Transfer->status);
	if (Transfer->type == LIBUSB_TRANSFER_TYPE_CONTROL)
	{
		memcpy(Request->m_UserPayload, Transfer->buffer + sizeof(USBSetup), Transfer->length - LIBUSB_CONTROL_SETUP_SIZE);
		delete[] Transfer->buffer;
	}

	if (Transfer->status != LIBUSB_TRANSFER_COMPLETED)
	{
		Request->Complete(-(1100 + Transfer->status));
	}
	else
	{
		if (Transfer->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
		{
			u16* PacketLengths = (u16*) Request->m_UserPayload;
			for (int i = 0; i < Transfer->num_iso_packets; i++)
			{
				libusb_iso_packet_descriptor* Desc = &Transfer->iso_packet_desc[i];
				PacketLengths[i] = Desc->status < 0 ? 0 : Desc->actual_length;
			}
		}
		Request->Complete(Transfer->actual_length);
	}
}

void CUSBDeviceReal::_ControlRequest(const USBSetup* Request, void* Payload, void* UserData)
{
	DEBUG_LOG(USBINTERFACE, "USBReal: control request");
	size_t Length = Common::swap16(Request->wLength);
	u8* Buf = new u8[sizeof(USBSetup) + Length];
	memcpy(Buf, Request, sizeof(USBSetup));
	memcpy(Buf + sizeof(USBSetup), Payload, Length);

	libusb_transfer* Transfer = libusb_alloc_transfer(0);
	CUSBRequest* URequest = new CUSBRequestReal(this, UserData, -1, Transfer, Payload);

	libusb_fill_control_transfer(Transfer, m_DeviceHandle, (unsigned char*) Buf, CUSBDeviceReal::TransferCallback, URequest, 0);
	int Err = libusb_submit_transfer(Transfer);
	if (Err)
	{
		URequest->Complete(-1002);
	}
}

void CUSBDeviceReal::BulkRequest(u8 Endpoint, size_t Length, void* Payload, void* UserData)
{
	DEBUG_LOG(USBINTERFACE, "USBReal: bulk request");
	libusb_transfer* Transfer = libusb_alloc_transfer(0);
	CUSBRequest* URequest = new CUSBRequestReal(this, UserData, Endpoint, Transfer);
	libusb_fill_bulk_transfer(Transfer, m_DeviceHandle, Endpoint, (unsigned char*) Payload, Length, TransferCallback, URequest, 0);
	int Err = libusb_submit_transfer(Transfer);
	if (Err)
	{
		URequest->Complete(-1002);
	}
}

void CUSBDeviceReal::InterruptRequest(u8 Endpoint, size_t Length, void* Payload, void* UserData)
{
	DEBUG_LOG(USBINTERFACE, "USBReal: interrupt request");
	libusb_transfer* Transfer = libusb_alloc_transfer(0);
	CUSBRequest* Request = new CUSBRequestReal(this, UserData, Endpoint, Transfer);
	libusb_fill_interrupt_transfer(Transfer, m_DeviceHandle, Endpoint, (unsigned char*) Payload, Length, TransferCallback, Request, 0);
	int Err = libusb_submit_transfer(Transfer);
	if (Err)
	{
		Request->Complete(-1002);
	}
}

void CUSBDeviceReal::IsochronousRequest(u8 Endpoint, size_t Length, size_t NumPackets, u16* PacketLengths, void* Payload, void* UserData)
{
	DEBUG_LOG(USBINTERFACE, "USBReal: isochronous request");
	libusb_transfer* Transfer = libusb_alloc_transfer(NumPackets);
	CUSBRequest* Request = new CUSBRequestReal(this, UserData, Endpoint, Transfer, PacketLengths);
	libusb_fill_iso_transfer(Transfer, m_DeviceHandle, Endpoint, (unsigned char*) Payload, Length, NumPackets, TransferCallback, Request, 0);
	for (size_t i = 0; i < NumPackets; i++)
	{
		Transfer->iso_packet_desc[i].length = PacketLengths[i];
	}
	int Err = libusb_submit_transfer(Transfer);
	if (Err)
	{
		Request->Complete(-1002);
	}

}

CUSBControllerReal::CUSBControllerReal()
{
	if (libusb_init(&m_UsbContext))
	{
		PanicAlert("Couldn't initialize libusb");
		return;
	}
	libusb_set_debug(m_UsbContext, 2);
#ifdef CUSBDEVICE_SUPPORTS_HOTPLUG
	m_HotplugHandle = NULL;
	m_UseHotplug = libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG);
#endif
	m_NextUid = 0;
	m_ShouldDestroy = false;

	m_Thread = new std::thread(&USBThreadFunc, this);
}

CUSBControllerReal::~CUSBControllerReal()
{
	for (auto itr = m_LastUidsRev.begin(); itr != m_LastUidsRev.end(); ++itr)
	{
		libusb_unref_device(itr->first);
	}
#ifdef CUSBDEVICE_SUPPORTS_HOTPLUG
	if (m_HotplugHandle != NULL)
	{
		libusb_hotplug_deregister_callback(m_UsbContext, m_HotplugHandle);
	}
#endif
	libusb_exit(m_UsbContext);
	m_Thread->detach();
	delete m_Thread;
}

void CUSBControllerReal::PollDevices(bool UseEvent)
{
	if (!g_ShouldScan) {
		return;
	}
	std::map<libusb_device*, u32> NewRev;
	std::map<u32, libusb_device*> NewUids;

	std::vector<USBDeviceDescriptorEtc> Results;
	libusb_device** List;
	ssize_t Count = libusb_get_device_list(m_UsbContext, &List);
	if (Count < 0)
	{
		DEBUG_LOG(USBINTERFACE, "libusb_get_device_list returned %zd", Count);
		return;
	}
	//|| (Count == m_OldCount && !memcmp(List, m_OldList, Count * sizeof(List))))
	for (ssize_t i = 0; i < Count; i++)
	{
		libusb_device* Device = List[i];
		auto itr = m_LastUidsRev.find(Device);
		u32 Uid;
		if (itr == m_LastUidsRev.end())
		{
			Uid = (UsbRealControllerId << 28) | (m_NextUid++ & 0x0fffffff);
		} else {
			Uid = itr->second;
		}
		NewRev[Device] = Uid;
		NewUids[Uid] = Device;

		std::vector<u8> DevBuffer;

		libusb_device_descriptor Desc;
		int Err = libusb_get_device_descriptor(Device, &Desc);
		if (Err)
		{
			WARN_LOG(USBINTERFACE, "libusb_get_device_descriptor failed with error: %d", Err);
			continue;
		}

		USBDeviceDescriptorEtc& WiiDevice = EmplaceBack(Results);
		memcpy(&WiiDevice, &Desc, sizeof(USBDeviceDescriptor));
		WiiDevice.Uid = Uid;

		for (u8 c = 0; c < Desc.bNumConfigurations; c++)
		{
			struct libusb_config_descriptor *Config = NULL;
			Err = libusb_get_config_descriptor(Device, c, &Config);

			USBConfigDescriptorEtc& WiiConfig = EmplaceBack(WiiDevice.Configs);
			memcpy(&WiiConfig, Config, sizeof(USBConfigDescriptor));
			WiiConfig.Rest.resize(Config->extra_length);
			memcpy(&WiiConfig.Rest[0], Config->extra, Config->extra_length);

			for (u8 ic = 0; ic < Config->bNumInterfaces; ic++)
			{
				const struct libusb_interface *interfaceContainer = &Config->interface[ic];
				for (int ia = 0; ia < interfaceContainer->num_altsetting; ia++)
				{
					const struct libusb_interface_descriptor *Interface = &interfaceContainer->altsetting[ia];
					if (Interface->bInterfaceClass == LIBUSB_CLASS_HID &&
						(Interface->bInterfaceProtocol == 1 || Interface->bInterfaceProtocol == 2))
					{
						// Mouse or keyboard.  Don't even try using this device, as it could mess up the host.
						Results.erase(Results.end() - 1);
						libusb_free_config_descriptor(Config);
						goto BadDevice;
					}


					USBInterfaceDescriptorEtc& WiiInterface = EmplaceBack(WiiConfig.Interfaces);
					memcpy(&WiiInterface, Interface, sizeof(USBInterfaceDescriptor));

					for (u8 ie = 0; ie < Interface->bNumEndpoints; ie++)
					{
						const struct libusb_endpoint_descriptor *Endpoint = &Interface->endpoint[ie];

						USBEndpointDescriptorEtc& WiiEndpoint = EmplaceBack(WiiInterface.Endpoints);
						memcpy(&WiiEndpoint, Endpoint, sizeof(USBEndpointDescriptor));
					}
				}
			}
			libusb_free_config_descriptor(Config);
		}

		BadDevice:;
	}


	for (auto itr = m_LastUidsRev.begin(); itr != m_LastUidsRev.end(); ++itr)
	{
		libusb_unref_device(itr->first);
	}

	std::lock_guard<std::mutex> Guard(m_PollResultsLock);
	m_LastUids.swap(NewUids);
	m_LastUidsRev.swap(NewRev);
	bool Different = m_PollResults != Results;
	DEBUG_LOG(USBINTERFACE, "USBReal: got %zu results, old %zu, %s", Results.size(), m_PollResults.size(), Different ? "different" : "same");
	if (Different)
	{
		m_PollResults.swap(Results);
		if (UseEvent)
		{
			CoreTiming::ScheduleEvent_Threadsafe_Immediate(g_USBInterfaceEvent, USBEventDevicesChanged);
		}
	}
}

void CUSBControllerReal::USBThread()
{
	timeval Tv;
	Tv.tv_sec = 0;
	Tv.tv_usec = 300000;
	while (1)
	{
		int Err = libusb_handle_events_timeout(m_UsbContext, &Tv);
		if (Err)
		{
			PanicAlert("libusb error %d", Err);
			return;
		}
		if (m_ShouldDestroy)
		{
			delete this;
			return;
		}
#ifdef CUSBDEVICE_SUPPORTS_HOTPLUG
		if (!m_UseHotplug)
		{
			PollDevices(true);
		}
#else
		PollDevices(true);
#endif
	}
}

void CUSBControllerReal::Destroy()
{
	m_ShouldDestroy = true;
}

#ifdef CUSBDEVICE_SUPPORTS_HOTPLUG
int CUSBControllerReal::HotplugCallback(libusb_context* Ctx, libusb_device* Device, libusb_hotplug_event Event, void* Data)
{
	CUSBControllerReal* Self = (CUSBControllerReal*) Data;
	Self->PollDevices(true);
	return 0;
}
#endif

void CUSBControllerReal::UpdateShouldScan()
{
#ifdef CUSBDEVICE_SUPPORTS_HOTPLUG
	if (m_UseHotplug)
	{
		if (g_ShouldScan)
		{
			int Err = libusb_hotplug_register_callback(
				m_UsbContext,
				LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
				LIBUSB_HOTPLUG_ENUMERATE,
				LIBUSB_HOTPLUG_MATCH_ANY,
				LIBUSB_HOTPLUG_MATCH_ANY,
				LIBUSB_HOTPLUG_MATCH_ANY,
				HotplugCallback,
				this,
				&m_HotplugHandle
			);
			if (Err)
			{
				PanicAlert("Couldn't ask libusb for hotplug events");
				return;
			}
		}
		else
		{
			if (m_HotplugHandle != NULL)

			{
				libusb_hotplug_deregister_callback(m_UsbContext, m_HotplugHandle);
				m_HotplugHandle = NULL;
			}
		}
	}
#endif

	if (g_ShouldScan)
	{
		// Ensure it's ready immediately
		PollDevices(false);
	}
}

void CUSBControllerReal::UpdateDeviceList(TDeviceList& List)
{
	std::lock_guard<std::mutex> Guard(m_PollResultsLock);
	List.insert(List.end(), m_PollResults.begin(), m_PollResults.end());
}

CUSBDeviceReal* CUSBControllerReal::OpenDevice(libusb_device* Device, u32 Uid, IUSBDeviceClient* Client)
{
	m_OpenedDevices.emplace(Device);
	libusb_device_handle* Handle;
	int Ret = libusb_open(Device, &Handle);
	if (Ret)
	{
		WARN_LOG(USBINTERFACE, "USBReal: libusb open %p failed", Device);
		return NULL;
	}

	CUSBDeviceReal* UDevice = new CUSBDeviceReal(Device, Uid, Handle, this, Client);
	UDevice->SetConfig(0);
	return UDevice;
}

IUSBDevice* CUSBControllerReal::OpenUid(u32 Uid, IUSBDeviceClient* Client)
{
	DEBUG_LOG(USBINTERFACE, "USBReal: open uid %u", Uid);
	std::lock_guard<std::mutex> Guard(m_PollResultsLock);
	auto itr = m_LastUids.find(Uid);
	if (itr == m_LastUids.end()) {
		return NULL;
	}
	libusb_device* Device = itr->second;
	if (m_OpenedDevices.find(Device) != m_OpenedDevices.end())
	{
		// Already open
		return NULL;
	}
	return OpenDevice(Device, Uid, Client);
}

} // interface
