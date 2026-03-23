
#include "BLEManager.h"
#include "SessionEventService.h"
#include "esp_bt.h"

static const char* TAG = "BLE_MGR";

BLEManager* BLEManager::_instance = nullptr;

RTC_NOINIT_ATTR uint8_t g_consecutive_bond_failures;
RTC_NOINIT_ATTR uint32_t g_ghost_bond_magic;

static constexpr uint32_t GHOST_BOND_MAGIC = 0xDEAD8008;


BLEServerCallbacksImpl::BLEServerCallbacksImpl(BLEManager* manager)
 : _manager(manager)
{
}

void BLEServerCallbacksImpl::onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
 _manager->onConnectInternal(desc);
}

void BLEServerCallbacksImpl::onDisconnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
 _manager->onDisconnectInternal(desc, 0);
}

uint32_t BLEServerCallbacksImpl::onPassKeyRequest {
 ESP_LOGI(TAG, "PassKey requested - using Just Works (0)");
 return 0;
}

void BLEServerCallbacksImpl::onAuthenticationComplete(ble_gap_conn_desc* desc) {
 bool bonded = desc->sec_state.bonded;
 bool encrypted = desc->sec_state.encrypted;
 
 ESP_LOGI(TAG, "Authentication complete - Bonded: %s, Encrypted: %s",
 bonded ? "Yes" : "No",
 encrypted ? "Yes" : "No");
 
 if (!bonded && !encrypted) {
 int bondCount = NimBLEDevice::getNumBonds;
 bool pairingWindowOpen = _manager->isPairingAllowed;

 ESP_LOGW(TAG, "Authentication failed - Bonded: No, Encrypted: No");
 ESP_LOGW(TAG, " Bond count: %d, Pairing window: %s",
 bondCount, pairingWindowOpen ? "OPEN" : "CLOSED");

 if (bondCount > 0 && !pairingWindowOpen) {
 ESP_LOGW(TAG, "STRANGER REJECTED");
 ESP_LOGW(TAG, " Reason: Device is bonded, auth failed, pairing closed");
 ESP_LOGW(TAG, " Action: Disconnecting to protect owner's device");
 ESP_LOGW(TAG, " Solution: Power cycle device to enable pairing");

 if (_manager->_eventHandler) {
 _manager->_eventHandler->onBLEAuthenticationComplete(false);
 _manager->_eventHandler->onBLEBonded(false);
 }

 NimBLEDevice::getServer->disconnect(desc->conn_handle);

 _manager->startAdvertising;
 return;
 }

 ESP_LOGW(TAG, "Keys missing/mismatched - NOT updating params");

 if (_manager->_eventHandler) {
 _manager->_eventHandler->onBLEAuthenticationComplete(false);
 _manager->_eventHandler->onBLEBonded(false);
 }
 return;
 }
 
 if (bonded && encrypted) {
 ESP_LOGI(TAG, "SECURE SESSION RESTORED (Keys Verified)");
 
 if (g_ghost_bond_magic == GHOST_BOND_MAGIC && g_consecutive_bond_failures > 0) {
 ESP_LOGI(TAG, "Encryption successful - resetting failure counter (was %u)",
 g_consecutive_bond_failures);
 g_consecutive_bond_failures = 0;
 }
 }
 
 _manager->_paramsNeedUpdate = true;
 ESP_LOGI(TAG, "Connection param update deferred (will apply in ~2s)");
 
 if (_manager->_eventHandler) {
 _manager->_eventHandler->onBLEAuthenticationComplete(bonded);
 _manager->_eventHandler->onBLEBonded(bonded);
 }
}

bool BLEServerCallbacksImpl::onConfirmPIN(uint32_t pin) {
 ESP_LOGI(TAG, "Confirm PIN: %06lu - Auto-accepting", pin);
 return true;
}


BLECharCallbacksImpl::BLECharCallbacksImpl(BLEManager* manager)
 : _manager(manager)
{
}

void BLECharCallbacksImpl::onWrite(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc) {
 
 const NimBLEAttValue& value = pCharacteristic->getValue;
 
 if (value.length == 0) {
 return;
 }
 
 const uint8_t* data = value.data;
 size_t len = value.length;
 
 bool isHandshakeMessage = false;
 
 if (len >= 2 && data[0] == 0xA5) {
 uint8_t msgType = data[1];
 
 if (msgType == 0x00 ||
 msgType == 0x02 ||
 msgType == 0x04)
 {
 isHandshakeMessage = true;
 }
 }
 
 if (desc != nullptr && !desc->sec_state.encrypted && !isHandshakeMessage) {
 uint8_t rejectedType = (len >= 2 && data[0] == 0xA5) ? data[1] : 0xFF;
 ESP_LOGW(TAG, "Write rejected (not encrypted) - MsgType: 0x%02X", rejectedType);
 
 if (!desc->sec_state.bonded) {
 ESP_LOGI(TAG, "Requesting security (encryption not complete)");
 
 int rc = NimBLEDevice::startSecurity(desc->conn_handle);
 if (rc != 0 && rc != BLE_HS_EALREADY) {
 ESP_LOGW(TAG, "startSecurity returned %d", rc);
 }
 }
 
 return;
 }
 
 if (isHandshakeMessage && desc != nullptr && !desc->sec_state.encrypted) {
 uint8_t msgType = (len >= 2) ? data[1] : 0xFF;
 ESP_LOGI(TAG, "Handshake message (0x%02X) allowed before encryption", msgType);
 }
 
 if (pCharacteristic == _manager->_pMainChar) {
 _manager->handleMainCharWrite(data, len);
 } else if (pCharacteristic == _manager->_pOtaChar) {
 _manager->handleOtaCharWrite(data, len);
 }
}

void BLECharCallbacksImpl::onRead(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc) {
 ESP_LOGD(TAG, "Characteristic read: %s", pCharacteristic->getUUID.toString.c_str);
}

void BLECharCallbacksImpl::onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc, uint16_t subValue) {
 if (pCharacteristic == _manager->_pMainChar) {
 _manager->_subscribed = (subValue > 0);
 ESP_LOGI(TAG, "Main characteristic %s", 
 _manager->_subscribed ? "SUBSCRIBED" : "UNSUBSCRIBED");

 if (_manager->_eventHandler) {
 _manager->_eventHandler->onBLESubscribe(_manager->_subscribed);
 }
 }
}


BLEManager::BLEManager
 : _pServer(nullptr)
 , _pMainService(nullptr)
 , _pGattService(nullptr)
 , _pMainChar(nullptr)
 , _pOtaChar(nullptr)
 , _pServiceChangedChar(nullptr)
 , _pDatabaseHashChar(nullptr)
 , _pAdvertising(nullptr)
 , _serverCallbacks(this)
 , _mainCharCallbacks(this)
 , _otaCharCallbacks(this)
 , _eventHandler(nullptr)
 , _historyService(nullptr)
 , _initialized(false)
 , _deviceConnected(false)
 , _pairingModeEnabled(false)
 , _subscribed(false)
 , _currentConnHandle(0xFFFF)
 , _connectionStartTime(0)
 , _pairingModeStartTime(0)
 , _securityRequested(false)
 , _paramsNeedUpdate(false)
 , _paramsUpdated(false)
 , _bootPairingWindowActive(false)
 , _bootTime(0)
 , _mux(portMUX_INITIALIZER_UNLOCKED)
{
}


bool BLEManager::init(HistoryService* historyService) {
 if (_initialized) {
 ESP_LOGW(TAG, "BLEManager already initialized");
 return true;
 }
 
 ESP_LOGI(TAG, "Initializing BLEManager with NimBLE");
 
 _historyService = historyService;
 
 if (g_ghost_bond_magic != GHOST_BOND_MAGIC) {
 g_ghost_bond_magic = GHOST_BOND_MAGIC;
 g_consecutive_bond_failures = 0;
 ESP_LOGI(TAG, "Cold boot - Ghost Bond counter initialized");
 } else {
 ESP_LOGI(TAG, "Warm boot - consecutive bond failures: %u",
 g_consecutive_bond_failures);
 }
 
 NimBLEDevice::init(DEVICE_NAME);
 
 NimBLEDevice::setPower(BLEParams::TX_POWER);
 
 esp_err_t err;
 err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, BLEParams::TX_POWER);
 if (err != ESP_OK) ESP_LOGE(TAG, "Failed to set ADV power: %d", err);
 
 err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, BLEParams::TX_POWER);
 if (err != ESP_OK) ESP_LOGE(TAG, "Failed to set SCAN power: %d", err);
 
 err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, BLEParams::TX_POWER);
 if (err != ESP_OK) ESP_LOGE(TAG, "Failed to set DEFAULT power: %d", err);
 
 #ifdef ESP_BLE_PWR_TYPE_CONN
 err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN, BLEParams::TX_POWER);
 if (err != ESP_OK) ESP_LOGE(TAG, "Failed to set CONN power: %d", err);
 #else
 ESP_LOGW(TAG, "ESP_BLE_PWR_TYPE_CONN not defined - relying on DEFAULT");
 #endif
 
 ESP_LOGI(TAG, "TX Power configured to level %d (Target: +9dBm)", BLEParams::TX_POWER);
 
 configureSecurity;
 
 NimBLEDevice::setMTU(BLEParams::REQUESTED_MTU);
 
 _pServer = NimBLEDevice::createServer;
 
 _pServer->setCallbacks(&_serverCallbacks);
 
 createServices;
 
 configureAdvertising;
 
 int bondCount = NimBLEDevice::getNumBonds;
 ESP_LOGI(TAG, "BLEManager initialized. Bonds: %d", bondCount);
 
 _initialized = true;
 
 _bootPairingWindowActive = true;
 _bootTime = millis;
 _pairingModeEnabled = true;
 
 bool ghostBondDetected = false;
 if (bondCount > 0 && g_consecutive_bond_failures >= 5) {
 ghostBondDetected = true;
 ESP_LOGW(TAG, "ORPHANED BOND DETECTED!");
 ESP_LOGW(TAG, " Consecutive failures: %u", g_consecutive_bond_failures);
 ESP_LOGW(TAG, " Current bonds: %d", bondCount);
 ESP_LOGW(TAG, " Action: Clearing bonds and enabling pairing mode");
 
 deleteAllBonds;
 bondCount = 0;
 
 g_consecutive_bond_failures = 0;
 }
 
 ESP_LOGI(TAG, "Boot Pairing Window OPEN");
 ESP_LOGI(TAG, " Duration: 60 seconds");
 ESP_LOGI(TAG, " Existing bonds: %d", bondCount);
 if (ghostBondDetected) {
 ESP_LOGI(TAG, " Ghost Bond Recovery: Active (bonds cleared)");
 }
 ESP_LOGI(TAG, " New connections: ALLOWED");
 
 startAdvertising;
 
 return true;
}

void BLEManager::setEventHandler(BLEEventHandler* handler) {
 _eventHandler = handler;
}


void BLEManager::configureSecurity {
 ESP_LOGI(TAG, "Configuring BLE security with bonding");
 
 NimBLEDevice::setSecurityAuth(true, false, true);
 
 NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
 
 NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
 NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
 
 ESP_LOGI(TAG, "Security configured: Bonding=Yes, MITM=No, SC=Yes, IRK=Yes");
}


void BLEManager::createServices {
 ESP_LOGI(TAG, "Creating GATT services");
 
 
 _pGattService = _pServer->createService(NimBLEUUID((uint16_t)GENERIC_ATTRIBUTE_SERVICE_UUID));
 
 _pServiceChangedChar = _pGattService->createCharacteristic(
 NimBLEUUID((uint16_t)SERVICE_CHANGED_CHAR_UUID),
 NIMBLE_PROPERTY::INDICATE
 );
 
 _pDatabaseHashChar = _pGattService->createCharacteristic(
 NimBLEUUID((uint16_t)DATABASE_HASH_CHAR_UUID),
 NIMBLE_PROPERTY::READ
 );
 
 updateDatabaseHash;
 
 _pGattService->start;
 ESP_LOGD(TAG, "Generic Attribute Service created with Database Hash");
 
 _pMainService = _pServer->createService(SERVICE_UUID);
 
 _pMainChar = _pMainService->createCharacteristic(
 CHAR_MAIN_UUID,
 NIMBLE_PROPERTY::READ |
 NIMBLE_PROPERTY::WRITE |
 NIMBLE_PROPERTY::WRITE_NR |
 NIMBLE_PROPERTY::NOTIFY |
 NIMBLE_PROPERTY::INDICATE
 );
 _pMainChar->setCallbacks(&_mainCharCallbacks);
 
 _pOtaChar = _pMainService->createCharacteristic(
 CHAR_OTA_UUID,
 NIMBLE_PROPERTY::WRITE_NR |
 NIMBLE_PROPERTY::NOTIFY
 );
 _pOtaChar->setCallbacks(&_otaCharCallbacks);

 if (!SessionEventService::getInstance->init(_pMainService)) {
 ESP_LOGW(TAG, "SessionEventService failed to initialize");
 }

 _pMainService->start;
 ESP_LOGI(TAG, "Main Service created with Main, OTA, and Session Event characteristics");
}


void BLEManager::configureAdvertising {
 _pAdvertising = NimBLEDevice::getAdvertising;
 
 _pAdvertising->addServiceUUID(SERVICE_UUID);
 
 _pAdvertising->setScanResponse(true);
 
 _pAdvertising->setAppearance(0x0340);
 
 _pAdvertising->setMinInterval(BLEParams::ADV_INTERVAL_MIN);
 _pAdvertising->setMaxInterval(BLEParams::ADV_INTERVAL_MAX);
 
 _pAdvertising->setMinPreferred(BLEParams::CONN_INTERVAL_MIN);
 _pAdvertising->setMaxPreferred(BLEParams::CONN_INTERVAL_MAX);
 
 ESP_LOGI(TAG, "Advertising configured: interval=%ums, UUID advertised, ScanResp=enabled",
 (BLEParams::ADV_INTERVAL_MIN * 625) / 1000);
}


void BLEManager::startAdvertising {
 if (!_initialized) return;
 
 if (!_deviceConnected) {
 _pAdvertising->start;
 ESP_LOGI(TAG, "Advertising STARTED");
 } else {
 ESP_LOGD(TAG, "Already connected, not starting advertising");
 }
}

void BLEManager::stopAdvertising {
 if (!_initialized) return;
 
 _pAdvertising->stop;
 ESP_LOGI(TAG, "Advertising STOPPED");
}

bool BLEManager::isAdvertising const {
 return _pAdvertising != nullptr && _pAdvertising->isAdvertising;
}


void BLEManager::disconnect {
 if (!_initialized || !_deviceConnected) return;
 
 ESP_LOGI(TAG, "Disconnecting device");
 
 if (_pServer->getConnectedCount > 0) {
 _pServer->disconnect(0);
 }
}

unsigned long BLEManager::getConnectionDuration const {
 if (!_deviceConnected) return 0;
 return millis - _connectionStartTime;
}

void BLEManager::onConnectInternal(ble_gap_conn_desc* desc) {
 char addrStr[18];
 snprintf(addrStr, sizeof(addrStr), "%02X:%02X:%02X:%02X:%02X:%02X",
 desc->peer_ota_addr.val[5], desc->peer_ota_addr.val[4],
 desc->peer_ota_addr.val[3], desc->peer_ota_addr.val[2],
 desc->peer_ota_addr.val[1], desc->peer_ota_addr.val[0]);
 

 int bondCount = NimBLEDevice::getNumBonds;
 ESP_LOGI(TAG, "Connection from %s - proceeding to security phase", addrStr);
 ESP_LOGI(TAG, " Existing bonds: %d, Pairing window: %s",
 bondCount, _pairingModeEnabled ? "OPEN" : "CLOSED");
 ESP_LOGI(TAG, " NOTE: Bonding status will be verified after authentication");
 
 portENTER_CRITICAL(&_mux);
 _deviceConnected = true;
 _currentConnHandle = desc->conn_handle;
 _connectionStartTime = millis;
 portEXIT_CRITICAL(&_mux);
 
 ESP_LOGI(TAG, "Device CONNECTED");
 ESP_LOGI(TAG, " Address: %s", addrStr);
 ESP_LOGI(TAG, " Handle: %u", _currentConnHandle);
 ESP_LOGI(TAG, " Bonded (pre-auth): %s", desc->sec_state.bonded ? "Yes" : "Pending verification");
 ESP_LOGI(TAG, " Encrypted: %s", desc->sec_state.encrypted ? "Yes" : "No");
 ESP_LOGI(TAG, " Pairing Window: %s", _pairingModeEnabled ? "OPEN" : "CLOSED");
 ESP_LOGI(TAG, " Existing Bonds: %d", bondCount);
 
 stopAdvertising;
 
 if (!desc->sec_state.bonded) {
 if (bondCount == 0) {
 ESP_LOGI(TAG, "No bonds exist. Initiating security proactively for first-time pairing.");
 _securityRequested = true;
 int rc = NimBLEDevice::startSecurity(desc->conn_handle);
 if (rc == 0) {
 ESP_LOGI(TAG, "Security initiation started successfully");
 } else if (rc == BLE_HS_EALREADY) {
 ESP_LOGI(TAG, "Security already in progress");
 } else {
 ESP_LOGW(TAG, "Failed to start security: %d", rc);
 }
 } else {
 ESP_LOGI(TAG, "Bonds exist (%d) but pre-auth bonded=false.", bondCount);
 ESP_LOGI(TAG, " Deferring security to iOS (awaiting IRK resolution).");
 ESP_LOGI(TAG, " If this is a returning bonded device, iOS will initiate encryption.");
 ESP_LOGI(TAG, " If this is a new device during pairing window, they can initiate pairing.");
 }
 } else {
 ESP_LOGI(TAG, "Device already bonded (pre-auth). Skipping security initiation.");
 }
 
 
 if (_eventHandler) {
 _eventHandler->onBLEConnect(_currentConnHandle);
 }
}

void BLEManager::onDisconnectInternal(ble_gap_conn_desc* desc, int reason) {
 unsigned long duration = millis - _connectionStartTime;
 uint16_t connHandle = desc ? desc->conn_handle : _currentConnHandle;
 
 portENTER_CRITICAL(&_mux);
 _deviceConnected = false;
 _subscribed = false;
 _securityRequested = false;
 _currentConnHandle = 0xFFFF;
 _paramsNeedUpdate = false;
 _paramsUpdated = false;
 portEXIT_CRITICAL(&_mux);
 
 ESP_LOGI(TAG, "Device DISCONNECTED");
 ESP_LOGI(TAG, " Reason: 0x%02X", reason);
 ESP_LOGI(TAG, " Duration: %lu seconds", duration / 1000);
 
 if (_eventHandler) {
 _eventHandler->onBLEDisconnect(connHandle, reason);
 }
 
 startAdvertising;
}

void BLEManager::updateConnectionParams(uint16_t connHandle) {
 
 _pServer->updateConnParams(
 connHandle,
 BLEParams::CONN_INTERVAL_MIN,
 BLEParams::CONN_INTERVAL_MAX,
 BLEParams::CONN_LATENCY,
 BLEParams::SUPERVISION_TIMEOUT
 );
 
 ESP_LOGI(TAG, "Connection params update requested (post-authentication)");
}


void BLEManager::enablePairingMode {
 _pairingModeEnabled = true;
 _pairingModeStartTime = millis;
 startAdvertising;
 ESP_LOGI(TAG, "Pairing mode ENABLED for %lu ms", Timing::PAIRING_WINDOW_MS);
}

void BLEManager::disablePairingMode {
 _pairingModeEnabled = false;
 ESP_LOGI(TAG, "Pairing mode DISABLED (bonded reconnect allowed)");
}

bool BLEManager::isPairingAllowed const {
 return (NimBLEDevice::getNumBonds == 0) || _pairingModeEnabled;
}

void BLEManager::deleteAllBonds {
 int bondCount = NimBLEDevice::getNumBonds;
 
 ESP_LOGI(TAG, "Deleting %d bonds", bondCount);
 
 for (int i = bondCount - 1; i >= 0; i--) {
 NimBLEAddress addr = NimBLEDevice::getBondedAddress(i);
 NimBLEDevice::deleteBond(addr);
 ESP_LOGI(TAG, " Deleted: %s", addr.toString.c_str);
 }
 
 ESP_LOGI(TAG, "All bonds deleted");
}

int BLEManager::getBondCount const {
 return NimBLEDevice::getNumBonds;
}


bool BLEManager::sendNotification(const uint8_t* data, size_t length) {
 if (!_initialized || !_deviceConnected || !_subscribed) {
 return false;
 }
 
 if (data == nullptr || length == 0) {
 return false;
 }
 
 _pMainChar->setValue(data, length);
 _pMainChar->notify;
 
 ESP_LOGD(TAG, "Notification sent: %u bytes", length);
 return true;
}

bool BLEManager::sendIndication(const uint8_t* data, size_t length) {
 if (!_initialized || !_deviceConnected || !_subscribed) {
 return false;
 }
 
 if (data == nullptr || length == 0) {
 return false;
 }
 
 _pMainChar->setValue(data, length);
 _pMainChar->indicate;
 
 ESP_LOGD(TAG, "Indication sent: %u bytes", length);
 return true;
}


void BLEManager::indicateServiceChanged {
 if (!_initialized || !_deviceConnected) return;
 
 uint8_t data[4];
 data[0] = 0x01;
 data[1] = 0x00;
 data[2] = 0xFF;
 data[3] = 0xFF;
 
 _pServiceChangedChar->setValue(data, 4);
 _pServiceChangedChar->indicate;
 
 ESP_LOGI(TAG, "Service Changed indication sent");
}

void BLEManager::updateDatabaseHash {
 if (!_pDatabaseHashChar) return;
 
 
 uint8_t hash[16] = {0};
 
 hash[0] = FIRMWARE_VERSION_MAJOR;
 hash[1] = FIRMWARE_VERSION_MINOR;
 hash[2] = FIRMWARE_VERSION_PATCH;
 hash[3] = FIRMWARE_BUILD_TYPE;
 
 const uint32_t GATT_STRUCTURE_VERSION = 0x00010001;
 hash[5] = (GATT_STRUCTURE_VERSION >> 16) & 0xFF;
 hash[6] = (GATT_STRUCTURE_VERSION >> 8) & 0xFF;
 hash[7] = GATT_STRUCTURE_VERSION & 0xFF;
 
 uint32_t uuidHash = 0;
 const char* svcUUID = SERVICE_UUID;
 while (*svcUUID) {
 uuidHash = (uuidHash * 31) + *svcUUID++;
 }
 
 hash[8] = (uuidHash >> 24) & 0xFF;
 hash[9] = (uuidHash >> 16) & 0xFF;
 hash[10] = (uuidHash >> 8) & 0xFF;
 hash[11] = uuidHash & 0xFF;
 
 hash[12] = 0x54;
 hash[13] = 0x52;
 hash[14] = 0x41;
 hash[15] = 0x4B;
 
 _pDatabaseHashChar->setValue(hash, 16);
 
 ESP_LOGI(TAG, "Database Hash updated: v%d.%d.%d build %d",
 hash[0], hash[1], hash[2], hash[3]);
}


void BLEManager::update {
 if (!_initialized) return;
 
 if (_bootPairingWindowActive) {
 if (millis - _bootTime > 60000) {
 _bootPairingWindowActive = false;
 
 int bondCount = NimBLEDevice::getNumBonds;
 
 if (bondCount > 0) {
 disablePairingMode;
 ESP_LOGI(TAG, "Boot Pairing Window CLOSED");
 ESP_LOGI(TAG, " Existing bonds: %d", bondCount);
 ESP_LOGI(TAG, " Pairing: CLOSED (bonded reconnect only)");
 ESP_LOGI(TAG, " Advertising: ON (for bonded devices)");
 } else {
 ESP_LOGW(TAG, "Boot Window expired but NO BONDS exist");
 ESP_LOGW(TAG, " Remaining in Pairing Mode to allow first-time setup");
 ESP_LOGW(TAG, " New connections: ALLOWED (until first bond)");
 }
 }
 }
 
 if (_pairingModeEnabled && !_bootPairingWindowActive) {
 if (_pairingModeStartTime > 0 && millis - _pairingModeStartTime > Timing::PAIRING_WINDOW_MS) {
 disablePairingMode;
 }
 }
 
 if (_deviceConnected && _paramsNeedUpdate && !_paramsUpdated) {
 unsigned long connectionAge = millis - _connectionStartTime;
 
 if (connectionAge >= 2000) {
 ESP_LOGI(TAG, "Applying deferred connection params (age: %lu ms)", connectionAge);
 
 updateConnectionParams(_currentConnHandle);
 
 _paramsUpdated = true;
 _paramsNeedUpdate = false;
 
 ESP_LOGI(TAG, "Connection params update requested successfully");
 }
 }

 if (_deviceConnected && !_securityRequested) {
 ble_gap_conn_desc desc;
 if (ble_gap_conn_find(_currentConnHandle, &desc) == 0) {
 if (!desc.sec_state.encrypted) {
 unsigned long connectionAge = millis - _connectionStartTime;
 if (connectionAge > 2500) {
 ESP_LOGW(TAG, "Security standoff detected (2.5s unencrypted). Forcing initiation.");
 _securityRequested = true;
 int rc = NimBLEDevice::startSecurity(_currentConnHandle);
 if (rc != 0 && rc != BLE_HS_EALREADY) {
 ESP_LOGW(TAG, "Forced startSecurity returned %d", rc);
 }
 }
 } else {
 _securityRequested = true;
 }
 }
 }
}

void BLEManager::processDataQueue {
 if (!_initialized || !_deviceConnected || !_subscribed) return;
 
 if (_historyService == nullptr) return;
 
 DetectionEvent event;
 if (_historyService->getNextUnsynced(&event)) {
 sendNotification(reinterpret_cast<uint8_t*>(&event), sizeof(DetectionEvent));
 
 ESP_LOGD(TAG, "Sent event ID=%lu via notification", event.id);
 }
}


void BLEManager::handleMainCharWrite(const uint8_t* data, size_t length) {
 ESP_LOGD(TAG, "Main char write: %u bytes", length);
 
 if (_eventHandler) {
 _eventHandler->onBLEDataReceived(data, length);
 }
}

void BLEManager::handleOtaCharWrite(const uint8_t* data, size_t length) {
 ESP_LOGD(TAG, "OTA char write: %u bytes", length);
 
 if (_eventHandler) {
 _eventHandler->onBLEDataReceived(data, length);
 }
}


void BLEManager::getDiagnostics(char* buffer, size_t bufferSize) const {
 snprintf(buffer, bufferSize,
 "BLE: init=%d, conn=%d, sub=%d, pair=%d, bonds=%d, adv=%d",
 _initialized,
 _deviceConnected,
 _subscribed,
 _pairingModeEnabled,
 NimBLEDevice::getNumBonds,
 isAdvertising);
}


void BLEManager::incrementGhostBondCounter {
 int bondCount = NimBLEDevice::getNumBonds;
 
 if (bondCount > 0 && g_ghost_bond_magic == GHOST_BOND_MAGIC && _deviceConnected) {
 g_consecutive_bond_failures++;
 
 ESP_LOGW(TAG, "Bond failure counter incremented to %u (threshold: 5)",
 g_consecutive_bond_failures);
 
 if (g_consecutive_bond_failures >= 5) {
 ESP_LOGW(TAG, "Threshold reached! Next boot will clear bonds.");
 }
 }
}

void BLEManager::resetGhostBondCounter {
 if (g_ghost_bond_magic == GHOST_BOND_MAGIC && g_consecutive_bond_failures > 0) {
 ESP_LOGI(TAG, "Ghost Bond counter explicitly reset (was %u)",
 g_consecutive_bond_failures);
 g_consecutive_bond_failures = 0;
 }
}
