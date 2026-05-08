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

#include <ble/Ble.h>
#include <devices/proximity-ranger/impl/BleRssiRangingAdapter.h>
#include <lib/support/logging/CHIPLogging.h>

#include <cstdint>

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_gap.h"
#elif defined(CONFIG_BT_BLUEDROID_ENABLED)
#include "esp_gap_ble_api.h"
#endif

/**
 * ESP32-specific BLE RSSI ranging adapter.
 *
 * Implements BLE scanning and beacon advertising for proximity ranging
 * on ESP32, supporting both NimBLE and Bluedroid BLE stacks via separate
 * source files compiled based on sdkconfig.
 */
class Esp32BleRssiRangingAdapter : public BleRssiRangingAdapter
{
public:
    static constexpr uint8_t kMaxConcurrentSessions = 4;

    /// Number of advertisement intervals before updating the beacon payload
    static constexpr uint8_t kAdvCountBeforeUpdate = 10;

    /// Advertisement interval range in 0.625ms units (300-500ms)
    static constexpr uint16_t kAdvIntervalMin = 480;  // 300ms / 0.625
    static constexpr uint16_t kAdvIntervalMax = 800;  // 500ms / 0.625

    /// Timer period for payload update: ~10 advertisements at ~400ms avg = 4000ms
    static constexpr uint32_t kAdvUpdateIntervalMs = 4000;

    enum class SessionRole : uint8_t
    {
        kScanning    = 0,
        kAdvertising = 1,
    };

    struct SessionState
    {
        bool active              = false;
        uint8_t sessionId        = 0;
        uint64_t peerBleDeviceId = 0;
        SessionRole role         = SessionRole::kScanning;
        uint16_t trackedMsgCounter = 0; // For scanning: last seen message counter from peer
        bool msgCounterInitialized = false; // Whether we've received the first message
    };

    chip::app::Clusters::ProximityRanging::ResultCodeEnum
    StartSession(uint8_t sessionId,
                 const chip::app::Clusters::ProximityRanging::Commands::StartRangingRequest::DecodableType & request) override;

    CHIP_ERROR StopSession(uint8_t sessionId) override;
    void StopAllSessions() override;
    CHIP_ERROR GetActiveSessionIds(std::vector<uint8_t> & sessionIds) override;

private:
    SessionState mSessions[kMaxConcurrentSessions];
    bool mScanning    = false;
    bool mAdvertising = false;
    uint16_t mAdvMsgCounter = 0;

    // --- Scanning ---
    CHIP_ERROR StartScanning();
    CHIP_ERROR StopScanning();
    void HandleAdvertisement(const uint8_t * advData, uint8_t advDataLen, int8_t rssi);
    void ProcessRangingBeacon(const chip::Ble::ChipBLEProximityRangingIdentificationInfo & payload, int8_t rssi);

    // --- Advertising ---
    CHIP_ERROR StartAdvertising();
    CHIP_ERROR StopAdvertising();
    CHIP_ERROR UpdateAdvertisingPayload();

    /// Timer callback to periodically update the advertising payload
    static void AdvUpdateTimerHandler(chip::System::Layer * layer, void * appState);
    void ScheduleAdvUpdate();
    void CancelAdvUpdate();

    // --- Session helpers ---
    SessionState * FindFreeSlot();
    SessionState * FindSession(uint8_t sessionId);
    bool HasActiveSessions() const;
    bool HasActiveSessionsWithRole(SessionRole role) const;

#ifdef CONFIG_BT_NIMBLE_ENABLED
    static int NimbleGapEventCallback(struct ble_gap_event * event, void * arg);
#elif defined(CONFIG_BT_BLUEDROID_ENABLED)
    static void BluedroidGapEventCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t * param);
    static Esp32BleRssiRangingAdapter * sInstance;
#endif
};
