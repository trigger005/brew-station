#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <functional>

// BLE client for the Varia AKU family of scales (AKU, AKU Mini, AKU Pro, AKU Micro).
//
// Protocol (from Beanconqueror's open-source driver):
//   Service  0xFFF0
//   Notify   0xFFF1  -> status frames from the scale
//   Write    0xFFF2  -> commands to the scale (write without response)
//
//   Weight frame:  FA 01 <len> W0 W1 W2 <xor>
//     sign  = (W0 & 0x10) ? -1 : +1
//     grams = sign * (((W0 & 0x0F) << 16) | (W1 << 8) | W2) / 100.0
//
//   Commands:      FA <cmd> 01 01 <cmd ^ 01 ^ 01>
//     0x82 tare, 0x88 timer start, 0x89 timer stop, 0x8A timer reset
class VariaAku {
public:
    using WeightCallback = std::function<void(float grams)>;
    using StateCallback = std::function<void(bool connected)>;

    void begin();
    // Call from loop(). Handles scanning, connecting and reconnecting.
    void loop();

    bool isConnected() const;
    float weight() const { return _weight; }
    uint32_t lastUpdateMs() const { return _lastUpdateMs; }

    bool tare();
    bool timerStart();
    bool timerStop();
    bool timerReset();

    void onWeight(WeightCallback cb) { _onWeight = cb; }
    void onConnectionChange(StateCallback cb) { _onState = cb; }

private:
    friend class AkuScanCallbacks;
    friend class AkuClientCallbacks;

    enum class State { Idle, Scanning, Found, Connected };

    bool connectToScale();
    bool sendCommand(uint8_t cmd, uint8_t arg = 0x01);
    void handleNotify(const uint8_t* data, size_t len);
    void startScan();

    static bool isAkuName(const std::string& name);

    volatile State _state = State::Idle;
    const NimBLEAdvertisedDevice* _found = nullptr;
    NimBLEClient* _client = nullptr;
    NimBLERemoteCharacteristic* _cmdChar = nullptr;

    volatile float _weight = 0.0f;
    volatile uint32_t _lastUpdateMs = 0;
    uint32_t _nextScanMs = 0;

    WeightCallback _onWeight;
    StateCallback _onState;
};
