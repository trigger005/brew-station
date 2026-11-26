#include "VariaAku.h"

static const NimBLEUUID kServiceUuid((uint16_t)0xFFF0);
static const NimBLEUUID kNotifyUuid((uint16_t)0xFFF1);
static const NimBLEUUID kWriteUuid((uint16_t)0xFFF2);

static constexpr uint32_t kScanDurationMs = 10000;
static constexpr uint32_t kRescanDelayMs = 2000;

static VariaAku* g_scale = nullptr;  // single instance, used by NimBLE callbacks

// ---------------------------------------------------------------- callbacks

class AkuScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!g_scale || g_scale->_state != VariaAku::State::Scanning) return;

        bool match = VariaAku::isAkuName(dev->getName()) || dev->isAdvertisingService(kServiceUuid);
        if (!match) {
            if (!dev->getName().empty()) {
                Serial.printf("[AKU]   seen \"%s\" (%s), RSSI %d\n", dev->getName().c_str(),
                              dev->getAddress().toString().c_str(), dev->getRSSI());
            }
            return;
        }

        Serial.printf("[AKU] Found \"%s\" (%s), RSSI %d\n", dev->getName().c_str(),
                      dev->getAddress().toString().c_str(), dev->getRSSI());
        g_scale->_found = dev;
        g_scale->_state = VariaAku::State::Found;
        NimBLEDevice::getScan()->stop();
    }

    void onScanEnd(const NimBLEScanResults&, int) override {
        if (g_scale && g_scale->_state == VariaAku::State::Scanning) {
            g_scale->_state = VariaAku::State::Idle;  // nothing found, loop() rescans
        }
    }
};

class AkuClientCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient*, int reason) override {
        Serial.printf("[AKU] Disconnected (reason %d)\n", reason);
        if (!g_scale) return;
        g_scale->_cmdChar = nullptr;
        g_scale->_state = VariaAku::State::Idle;
        g_scale->_nextScanMs = millis() + kRescanDelayMs;
        if (g_scale->_onState) g_scale->_onState(false);
    }
};

static AkuScanCallbacks s_scanCb;
static AkuClientCallbacks s_clientCb;

// ---------------------------------------------------------------- public API

void VariaAku::begin() {
    g_scale = this;
    NimBLEDevice::init("coffee-station");
    NimBLEDevice::setPower(9);  // dBm, max for ESP32-S3

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_scanCb, false);
    scan->setActiveScan(true);  // needed to receive the device name in the scan response
    scan->setInterval(100);
    scan->setWindow(99);

    _state = State::Idle;
    _nextScanMs = 0;
}

void VariaAku::loop() {
    switch (_state) {
        case State::Idle:
            if ((int32_t)(millis() - _nextScanMs) >= 0) startScan();
            break;

        case State::Found:
            if (connectToScale()) {
                _state = State::Connected;
                if (_onState) _onState(true);
            } else {
                _state = State::Idle;
                _nextScanMs = millis() + kRescanDelayMs;
            }
            break;

        case State::Scanning:
        case State::Connected:
            break;
    }
}

bool VariaAku::isConnected() const {
    return _state == State::Connected && _client && _client->isConnected();
}

bool VariaAku::tare() { return sendCommand(0x82); }
bool VariaAku::timerStart() { return sendCommand(0x88); }
bool VariaAku::timerStop() { return sendCommand(0x89); }
bool VariaAku::timerReset() { return sendCommand(0x8A); }

// ---------------------------------------------------------------- internals

bool VariaAku::isAkuName(const std::string& name) {
    String n(name.c_str());
    n.toLowerCase();
    return n.indexOf("varia aku") >= 0 || n.indexOf("aku mini") >= 0 || n.indexOf("aku") == 0;
}

void VariaAku::startScan() {
    Serial.println("[AKU] Scanning...");
    _found = nullptr;
    _state = State::Scanning;
    NimBLEDevice::getScan()->clearResults();
    if (!NimBLEDevice::getScan()->start(kScanDurationMs, false, true)) {
        Serial.println("[AKU] Failed to start scan");
        _state = State::Idle;
        _nextScanMs = millis() + kRescanDelayMs;
    }
}

bool VariaAku::connectToScale() {
    if (!_found) return false;

    if (!_client) {
        _client = NimBLEDevice::createClient();
        _client->setClientCallbacks(&s_clientCb, false);
        _client->setConnectionParams(12, 12, 0, 150);  // 15 ms interval, 1.5 s timeout
        _client->setConnectTimeout(5000);
    }

    Serial.println("[AKU] Connecting...");
    if (!_client->connect(_found)) {
        Serial.println("[AKU] Connect failed");
        return false;
    }

    NimBLERemoteService* svc = _client->getService(kServiceUuid);
    if (!svc) {
        Serial.println("[AKU] Service FFF0 not found");
        _client->disconnect();
        return false;
    }

    NimBLERemoteCharacteristic* notifyChar = svc->getCharacteristic(kNotifyUuid);
    _cmdChar = svc->getCharacteristic(kWriteUuid);
    if (!notifyChar || !_cmdChar) {
        Serial.println("[AKU] Characteristics FFF1/FFF2 not found");
        _client->disconnect();
        return false;
    }

    bool ok = notifyChar->subscribe(
        notifyChar->canNotify(),  // notify if supported, otherwise indicate
        [this](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) { handleNotify(data, len); });
    if (!ok) {
        Serial.println("[AKU] Subscribe failed");
        _client->disconnect();
        return false;
    }

    Serial.println("[AKU] Connected and subscribed");
    return true;
}

bool VariaAku::sendCommand(uint8_t cmd, uint8_t arg) {
    if (!isConnected() || !_cmdChar) return false;
    uint8_t frame[5] = {0xFA, cmd, 0x01, arg, (uint8_t)(cmd ^ 0x01 ^ arg)};
    return _cmdChar->writeValue(frame, sizeof(frame), false);
}

void VariaAku::handleNotify(const uint8_t* d, size_t len) {
    if (len < 6 || d[0] != 0xFA) return;

    if (d[1] == 0x01) {  // weight
        int32_t raw = ((int32_t)(d[3] & 0x0F) << 16) | ((int32_t)d[4] << 8) | d[5];
        if (d[3] & 0x10) raw = -raw;
        _weight = raw / 100.0f;
        _lastUpdateMs = millis();
        if (_onWeight) _onWeight(_weight);
    }
    // Other frame types (battery, timer, ...) are ignored for now.
}
