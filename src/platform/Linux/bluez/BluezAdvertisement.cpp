/*
 *
 *    Copyright (c) 2020-2023 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "BluezAdvertisement.h"

#include <memory>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

#include <ble/Ble.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/ConfigurationManager.h>
#include <platform/GLibTypeDeleter.h>
#include <platform/Linux/dbus/bluez/DBusBluez.h>
#include <platform/PlatformManager.h>

#include "BluezEndpoint.h"
#include "Types.h"

namespace chip {
namespace DeviceLayer {
namespace Internal {

BluezLEAdvertisement1 * BluezAdvertisement::CreateLEAdvertisement()
{
    BluezLEAdvertisement1 * adv;
    BluezObjectSkeleton * object;

    ChipLogDetail(DeviceLayer, "Create BLE adv object at %s", mAdvPath);
    object = bluez_object_skeleton_new(mAdvPath);

    adv = bluez_leadvertisement1_skeleton_new();

    bluez_leadvertisement1_set_type_(adv, mIsBroadcast ? "broadcast" : "peripheral");

    if (!mIsBroadcast)
    {
        GVariantBuilder serviceUUIDsBuilder;
        g_variant_builder_init(&serviceUUIDsBuilder, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&serviceUUIDsBuilder, "s", mAdvUUID);
        bluez_leadvertisement1_set_service_uuids(adv, g_variant_builder_end(&serviceUUIDsBuilder));

        // Setting "Discoverable" to False on the adapter and to True on the advertisement convinces
        // Bluez to set "BR/EDR Not Supported" flag. Bluez doesn't provide API to do that explicitly
        // and the flag is necessary to force using LE transport.
        bluez_leadvertisement1_set_discoverable(adv, TRUE);
        // empty discoverable timeout for infinite discoverability

        // empty includes
        bluez_leadvertisement1_set_local_name(adv, mAdvName);
        bluez_leadvertisement1_set_appearance(adv, 0xffff /* no appearance */);
    }
    // empty duration
    // empty timeout
    // empty secondary channel for now

    bluez_object_skeleton_set_leadvertisement1(object, adv);
    g_signal_connect(adv, "handle-release",
                     G_CALLBACK(+[](BluezLEAdvertisement1 * aAdv, GDBusMethodInvocation * aInv, BluezAdvertisement * self) {
                         return self->BluezLEAdvertisement1Release(aAdv, aInv);
                     }),
                     this);

    g_dbus_object_manager_server_export(GetObjectManager(), G_DBUS_OBJECT_SKELETON(object));
    g_object_unref(object);

    return adv;
}

gboolean BluezAdvertisement::BluezLEAdvertisement1Release(BluezLEAdvertisement1 * aAdv, GDBusMethodInvocation * aInvocation)
{
    // This method is called when the advertisement is stopped (released) by BlueZ.
    // We can use it to update the state of the advertisement in the CHIP layer.
    ChipLogDetail(DeviceLayer, "BLE advertisement stopped by BlueZ");
    mIsAdvertising = false;
    bluez_leadvertisement1_complete_release(aAdv, aInvocation);
    if (mDelegate)
    {
        mDelegate->OnAdvertisementReleased();
    }
    else
    {
        BLEManagerImpl::NotifyBLEPeripheralAdvReleased();
    }
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

CHIP_ERROR BluezAdvertisement::InitImpl()
{
    // When creating D-Bus proxy object, the thread default context must be initialized. Otherwise,
    // all D-Bus signals will be delivered to the GLib global default main context.
    VerifyOrDie(g_main_context_get_thread_default() != nullptr);

    if (mIsBroadcast && mOwnObjectManager == nullptr)
    {
        mOwnObjectManager = g_dbus_object_manager_server_new("/chip/proximity");
        GDBusConnection * conn = g_dbus_proxy_get_connection(G_DBUS_PROXY(mAdapter.get()));
        if (conn != nullptr)
        {
            g_dbus_object_manager_server_set_connection(mOwnObjectManager, conn);
            ChipLogProgress(DeviceLayer, "Broadcast adv: object manager connected to D-Bus");
        }
        else
        {
            ChipLogError(DeviceLayer, "Broadcast adv: no D-Bus connection from adapter proxy!");
        }
    }

    mAdv.reset(CreateLEAdvertisement());
    return CHIP_NO_ERROR;
}

CHIP_ERROR BluezAdvertisement::Init(BluezAdapter1 * apAdapter, const char * aAdvUUID, const char * aAdvName, bool aBroadcast)
{
    VerifyOrReturnError(!mAdv, CHIP_ERROR_INCORRECT_STATE,
                        ChipLogError(DeviceLayer, "FAIL: BLE advertisement already initialized in %s", __func__));

    mAdapter.reset(reinterpret_cast<BluezAdapter1 *>(g_object_ref(apAdapter)));
    mIsBroadcast = aBroadcast;

    if (mIsBroadcast)
    {
        // Broadcast advertisements don't need GATT — use our own object manager path
        g_snprintf(mAdvPath, sizeof(mAdvPath), "/chip/proximity/adv");
    }
    else
    {
        GAutoPtr<char> rootPath;
        g_object_get(G_OBJECT(mEndpoint.GetGattApplicationObjectManager()), "object-path", &rootPath.GetReceiver(), nullptr);
        g_snprintf(mAdvPath, sizeof(mAdvPath), "%s/advertising", rootPath.get());
    }
    g_strlcpy(mAdvUUID, aAdvUUID, sizeof(mAdvUUID));

    if (aAdvName != nullptr)
    {
        g_strlcpy(mAdvName, aAdvName, sizeof(mAdvName));
    }
    else
    {
        // Advertising name corresponding to the PID, for debug purposes.
        g_snprintf(mAdvName, sizeof(mAdvName), "%s%04x", CHIP_DEVICE_CONFIG_BLE_DEVICE_NAME_PREFIX, getpid() & 0xffff);
    }

    CHIP_ERROR err = PlatformMgrImpl().GLibMatterContextInvokeSync(
        +[](BluezAdvertisement * self) { return self->InitImpl(); }, this);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err,
                        ChipLogError(Ble, "Failed to schedule BLE advertisement Init() on CHIPoBluez thread"));

    mIsInitialized = true;
    return CHIP_NO_ERROR;
}

CHIP_ERROR BluezAdvertisement::SetIntervals(AdvertisingIntervals aAdvIntervals)
{
    VerifyOrReturnError(mAdv, CHIP_ERROR_UNINITIALIZED);
    // If the advertisement is already running, BlueZ will update the intervals
    // automatically. There is no need to stop and restart the advertisement.
    bluez_leadvertisement1_set_min_interval(mAdv.get(), aAdvIntervals.first * 0.625);
    bluez_leadvertisement1_set_max_interval(mAdv.get(), aAdvIntervals.second * 0.625);
    return CHIP_NO_ERROR;
}

GDBusObjectManagerServer * BluezAdvertisement::GetObjectManager() const
{
    if (mOwnObjectManager != nullptr)
    {
        return mOwnObjectManager;
    }
    return mEndpoint.GetGattApplicationObjectManager();
}

CHIP_ERROR BluezAdvertisement::SetServiceDataRaw(chip::ByteSpan data)
{
    VerifyOrReturnError(mAdv, CHIP_ERROR_UNINITIALIZED);

    ChipLogProgress(DeviceLayer, "SetServiceDataRaw: %u bytes for UUID %s", static_cast<unsigned>(data.size()), mAdvUUID);

    GVariantBuilder serviceDataBuilder;
    g_variant_builder_init(&serviceDataBuilder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&serviceDataBuilder, "{sv}", mAdvUUID,
                          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.data(), data.size(), sizeof(uint8_t)));

    GVariant * serviceData = g_variant_builder_end(&serviceDataBuilder);

    GAutoPtr<char> debugStr(g_variant_print(serviceData, TRUE));
    ChipLogProgress(DeviceLayer, "SetServiceDataRaw: %s", StringOrNullMarker(debugStr.get()));

    bluez_leadvertisement1_set_service_data(mAdv.get(), serviceData);

    return CHIP_NO_ERROR;
}

CHIP_ERROR BluezAdvertisement::SetupServiceData(ServiceDataFlags aFlags)
{
    VerifyOrReturnError(mAdv, CHIP_ERROR_UNINITIALIZED);

    Ble::ChipBLEDeviceIdentificationInfo deviceInfo;
    ReturnErrorOnFailure(ConfigurationMgr().GetBLEDeviceIdentificationInfo(deviceInfo));

#if CHIP_ENABLE_ADDITIONAL_DATA_ADVERTISING
    deviceInfo.SetAdditionalDataFlag(true);
#endif

#if CHIP_DEVICE_CONFIG_EXT_ADVERTISING
    if (aFlags & kServiceDataExtendedAnnouncement)
    {
        deviceInfo.SetExtendedAnnouncementFlag(true);
        // In case of extended advertisement, specification requires that
        // the vendor ID and product ID are set to 0.
        deviceInfo.SetVendorId(0);
        deviceInfo.SetProductId(0);
    }
#endif

    GVariantBuilder serviceDataBuilder;
    g_variant_builder_init(&serviceDataBuilder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&serviceDataBuilder, "{sv}", mAdvUUID,
                          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, &deviceInfo, sizeof(deviceInfo), sizeof(uint8_t)));

    GVariant * serviceData = g_variant_builder_end(&serviceDataBuilder);

    GAutoPtr<char> debugStr(g_variant_print(serviceData, TRUE));
    ChipLogDetail(DeviceLayer, "SET service data to %s", StringOrNullMarker(debugStr.get()));

    bluez_leadvertisement1_set_service_data(mAdv.get(), serviceData);

    return CHIP_NO_ERROR;
}

void BluezAdvertisement::Shutdown()
{
    VerifyOrReturn(mIsInitialized);

    // Make sure that the advertisement is stopped before we start cleaning up.
    if (mIsAdvertising)
    {
        CHIP_ERROR err = Stop();
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "Failed to stop BLE advertisement before shutdown: %" CHIP_ERROR_FORMAT, err.Format());
        }
    }

    // Release resources on the glib thread to synchronize with potential signal handlers
    // attached to the advertising object that may run on the glib thread.
    TEMPORARY_RETURN_IGNORED PlatformMgrImpl().GLibMatterContextInvokeSync(
        +[](BluezAdvertisement * self) {
            // The application object manager might not be released right away (it may be held
            // by other BLE layer objects). We need to unexport the advertisement object in the
            // explicit way to make sure that we can export it again in the Init() method.
            g_dbus_object_manager_server_unexport(self->GetObjectManager(), self->mAdvPath);
            self->mAdapter.reset();
            self->mAdv.reset();
            if (self->mOwnObjectManager != nullptr)
            {
                g_object_unref(self->mOwnObjectManager);
                self->mOwnObjectManager = nullptr;
            }
            return CHIP_NO_ERROR;
        },
        this);

    mIsInitialized = false;
}

void BluezAdvertisement::StartDone(GObject * aObject, GAsyncResult * aResult)
{
    GAutoPtr<GError> error;
    if (!bluez_leadvertising_manager1_call_register_advertisement_finish(reinterpret_cast<BluezLEAdvertisingManager1 *>(aObject),
                                                                         aResult, &error.GetReceiver()))
    {
        ChipLogError(DeviceLayer, "FAIL: RegisterAdvertisement: %s", error->message);
        if (mDelegate)
        {
            mDelegate->OnAdvertisementStartComplete(BluezCallToChipError(error.get()));
        }
        else
        {
            BLEManagerImpl::NotifyBLEPeripheralAdvStartComplete(BluezCallToChipError(error.get()));
        }
        return;
    }

    mIsAdvertising = true;

    ChipLogDetail(DeviceLayer, "BLE advertisement started successfully");
    if (mDelegate)
    {
        mDelegate->OnAdvertisementStartComplete(CHIP_NO_ERROR);
    }
    else
    {
        BLEManagerImpl::NotifyBLEPeripheralAdvStartComplete(CHIP_NO_ERROR);
    }
}

CHIP_ERROR BluezAdvertisement::StartImpl()
{
    VerifyOrReturnError(mAdapter, CHIP_ERROR_UNINITIALIZED);

    // If the adapter configured in the Init() was unplugged, the g_dbus_interface_get_object()
    // or bluez_object_get_leadvertising_manager1() might return nullptr (depending on the timing,
    // since the D-Bus communication is handled on a separate thread). In such case, we should not
    // report internal error, but adapter unavailable, so the application can handle the situation
    // properly.

    GDBusObject * adapterObject = g_dbus_interface_get_object(reinterpret_cast<GDBusInterface *>(mAdapter.get()));
    VerifyOrReturnError(adapterObject != nullptr, BLE_ERROR_ADAPTER_UNAVAILABLE);
    GAutoPtr<BluezLEAdvertisingManager1> advMgr(
        bluez_object_get_leadvertising_manager1(reinterpret_cast<BluezObject *>(adapterObject)));
    VerifyOrReturnError(advMgr, BLE_ERROR_ADAPTER_UNAVAILABLE);

    GVariantBuilder optionsBuilder;
    g_variant_builder_init(&optionsBuilder, G_VARIANT_TYPE("a{sv}"));
    GVariant * options = g_variant_builder_end(&optionsBuilder);

    bluez_leadvertising_manager1_call_register_advertisement(
        advMgr.get(), mAdvPath, options, nullptr,
        [](GObject * aObject, GAsyncResult * aResult, void * aData) {
            reinterpret_cast<BluezAdvertisement *>(aData)->StartDone(aObject, aResult);
        },
        this);

    return CHIP_NO_ERROR;
}

CHIP_ERROR BluezAdvertisement::Start()
{
    VerifyOrReturnError(mIsInitialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnValue(!mIsAdvertising, CHIP_NO_ERROR, ChipLogDetail(DeviceLayer, "BLE advertising already started"));
    return PlatformMgrImpl().GLibMatterContextInvokeSync(
        +[](BluezAdvertisement * self) { return self->StartImpl(); }, this);
}

void BluezAdvertisement::StopDone(GObject * aObject, GAsyncResult * aResult)
{
    GAutoPtr<GError> error;
    if (!bluez_leadvertising_manager1_call_unregister_advertisement_finish(reinterpret_cast<BluezLEAdvertisingManager1 *>(aObject),
                                                                           aResult, &error.GetReceiver()))
    {
        ChipLogError(DeviceLayer, "FAIL: UnregisterAdvertisement: %s", error->message);
        if (mDelegate)
        {
            mDelegate->OnAdvertisementStopComplete(BluezCallToChipError(error.get()));
        }
        else
        {
            BLEManagerImpl::NotifyBLEPeripheralAdvStopComplete(BluezCallToChipError(error.get()));
        }
        return;
    }

    mIsAdvertising = false;

    ChipLogDetail(DeviceLayer, "BLE advertisement stopped successfully");
    if (mDelegate)
    {
        mDelegate->OnAdvertisementStopComplete(CHIP_NO_ERROR);
    }
    else
    {
        BLEManagerImpl::NotifyBLEPeripheralAdvStopComplete(CHIP_NO_ERROR);
    }
}

CHIP_ERROR BluezAdvertisement::StopImpl()
{
    VerifyOrReturnError(mAdapter, CHIP_ERROR_UNINITIALIZED);

    // If the adapter configured in the Init() was unplugged, the g_dbus_interface_get_object()
    // or bluez_object_get_leadvertising_manager1() might return nullptr (depending on the timing,
    // since the D-Bus communication is handled on a separate thread). In such case, we should not
    // report internal error, but adapter unavailable, so the application can handle the situation
    // properly.

    GDBusObject * adapterObject = g_dbus_interface_get_object(reinterpret_cast<GDBusInterface *>(mAdapter.get()));
    VerifyOrReturnError(adapterObject != nullptr, BLE_ERROR_ADAPTER_UNAVAILABLE);
    GAutoPtr<BluezLEAdvertisingManager1> advMgr(
        bluez_object_get_leadvertising_manager1(reinterpret_cast<BluezObject *>(adapterObject)));
    VerifyOrReturnError(advMgr, BLE_ERROR_ADAPTER_UNAVAILABLE);

    bluez_leadvertising_manager1_call_unregister_advertisement(
        advMgr.get(), mAdvPath, nullptr,
        [](GObject * aObject, GAsyncResult * aResult, void * aData) {
            reinterpret_cast<BluezAdvertisement *>(aData)->StopDone(aObject, aResult);
        },
        this);

    return CHIP_NO_ERROR;
}

CHIP_ERROR BluezAdvertisement::Stop()
{
    VerifyOrReturnError(mIsInitialized, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnValue(mIsAdvertising, CHIP_NO_ERROR, ChipLogDetail(DeviceLayer, "BLE advertising already stopped"));
    return PlatformMgrImpl().GLibMatterContextInvokeSync(
        +[](BluezAdvertisement * self) { return self->StopImpl(); }, this);
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip
