/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
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
#pragma once

#include <cstdint>
#include <vector>

#include <ble/Ble.h>
#include <devices/proximity-ranger/impl/BleRssiRangingAdapter.h>
#include <lib/core/CHIPError.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/Linux/bluez/BluezAdvertisement.h>
#include <platform/Linux/bluez/BluezEndpoint.h>
#include <platform/Linux/bluez/BluezObjectManager.h>
#include <platform/Linux/bluez/ChipDeviceScanner.h>

/**
 * Linux-specific BLE RSSI ranging adapter using BlueZ platform classes.
 *
 * Uses ChipDeviceScanner for passive BLE scanning and BluezAdvertisement
 * for non-connectable beacon advertising.
 */
class LinuxBleRssiRangingAdapter : public BleRssiRangingAdapter,
                                   public chip::DeviceLayer::Internal::ChipDeviceScannerDelegate,
                                   public chip::DeviceLayer::Internal::BluezAdvertisementDelegate
{
public:
    static constexpr uint8_t kMaxConcurrentSessions = 4;
    static constexpr uint16_t kAdvIntervalMin = 480;  // 300ms in 0.625ms units
    static constexpr uint16_t kAdvIntervalMax = 800;  // 500ms in 0.625ms units
    static constexpr uint32_t kAdvUpdateIntervalMs = 4000;

    enum class SessionRole : uint8_t
    {
        kScanning    = 0,
        kAdvertising = 1,
    };

    struct SessionState
    {
        bool active                = false;
        uint8_t sessionId          = 0;
        uint64_t peerBleDeviceId   = 0;
        SessionRole role           = SessionRole::kScanning;
        uint16_t trackedMsgCounter = 0;
        bool msgCounterInitialized = false;
    };

    // --- RangingAdapter interface ---
    chip::app::Clusters::ProximityRanging::ResultCodeEnum
    StartSession(uint8_t sessionId,
                 const chip::app::Clusters::ProximityRanging::Commands::StartRangingRequest::DecodableType & request) override;
    CHIP_ERROR StopSession(uint8_t sessionId) override;
    void StopAllSessions() override;
    CHIP_ERROR GetActiveSessionIds(std::vector<uint8_t> & sessionIds) override;

    // --- ChipDeviceScannerDelegate ---
    void OnDeviceScanned(BluezDevice1 & device, const chip::Ble::ChipBLEDeviceIdentificationInfo & info) override {}
    void OnDeviceScanned(BluezDevice1 & device, const chip::Ble::ChipBLEProximityRangingIdentificationInfo & info,
                         int8_t rssi) override;
    void OnScanComplete() override {}
    void OnScanError(CHIP_ERROR err) override;

    // --- BluezAdvertisementDelegate ---
    void OnAdvertisementReleased() override;

private:
    SessionState mSessions[kMaxConcurrentSessions];
    bool mScanning    = false;
    bool mAdvertising = false;
    uint16_t mAdvMsgCounter = 0;
    bool mBleInitialized = false;

    // Platform BLE objects
    chip::DeviceLayer::Internal::BluezObjectManager mObjectManager;
    chip::DeviceLayer::Internal::BluezEndpoint mEndpoint{ mObjectManager };
    chip::DeviceLayer::Internal::BluezAdvertisement mAdvertisement{ mEndpoint };
    chip::DeviceLayer::Internal::ChipDeviceScanner mScanner{ mObjectManager };

    // --- BLE lifecycle ---
    CHIP_ERROR EnsureBleInitialized();

    // --- Scanning ---
    CHIP_ERROR StartScanning();
    CHIP_ERROR StopScanning();
    void ProcessRangingBeacon(const chip::Ble::ChipBLEProximityRangingIdentificationInfo & payload, int8_t rssi);

    // --- Advertising ---
    CHIP_ERROR StartAdvertising();
    CHIP_ERROR StopAdvertising();
    CHIP_ERROR UpdateAdvertisingPayload();

    static void AdvUpdateTimerHandler(chip::System::Layer * layer, void * appState);
    void ScheduleAdvUpdate();
    void CancelAdvUpdate();

    // --- Session helpers ---
    SessionState * FindFreeSlot();
    SessionState * FindSession(uint8_t sessionId);
    bool HasActiveSessionsWithRole(SessionRole role) const;
};
