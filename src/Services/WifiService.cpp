
#include "WifiService.h"
#include <esp_wifi.h>

static const char* TAG = "WIFI";


WifiService* WifiService::_instance = nullptr;

WifiService* WifiService::getInstance {
 if (_instance == nullptr) {
 _instance = new WifiService;
 }
 return _instance;
}


WifiService::WifiService
 : _state(WifiState::WIFI_DISABLED)
 , _ssid("")
 , _password("")
 , _lastConnectionAttempt(0)
 , _connectionStartTime(0)
 , _retryCount(0)
 , _initialized(false)
 , _onConnectionChange(nullptr)
 , _onStatusChange(nullptr)
 , _lastDisconnectReason(0)
{
 ESP_LOGI(TAG, "WifiService constructed");
}


void WifiService::init {
 if (_initialized) {
 ESP_LOGW(TAG, "WifiService already initialized");
 return;
 }
 
 ESP_LOGI(TAG, "Initializing WifiService (PASSIVE MODE - One-Shot Policy)...");
 
 Preferences prefs;
 if (prefs.begin(WifiConfig::NVS_NAMESPACE, true)) {
 _ssid = prefs.getString(WifiConfig::NVS_KEY_SSID, "");
 _password = prefs.getString(WifiConfig::NVS_KEY_PASS, "");
 prefs.end;
 
 if (_ssid.length > 0) {
 ESP_LOGI(TAG, "Loaded credentials for SSID: %s", _ssid.c_str);
 } else {
 ESP_LOGI(TAG, "No stored credentials found");
 }
 } else {
 ESP_LOGW(TAG, "Failed to open NVS for credentials");
 }
 
 
 _initialized = true;
 
 if (_ssid.length > 0) {
 _state = WifiState::DISCONNECTED;
 ESP_LOGI(TAG, "Credentials found - READY (not connecting yet)");
 } else {
 _state = WifiState::WIFI_DISABLED;
 ESP_LOGI(TAG, "No credentials - Wi-Fi disabled");
 }
 
 ESP_LOGI(TAG, "WifiService initialized (PASSIVE). State: %s", getStateName);
}


void WifiService::update {
 if (!_initialized) {
 return;
 }
 
 if (_state == WifiState::DISCONNECTED || _state == WifiState::WIFI_DISABLED) {
 wifi_mode_t mode;
 esp_wifi_get_mode(&mode);
 if (mode == WIFI_MODE_NULL) {
 return;
 }
 }
 
 unsigned long now = millis;
 wl_status_t wifiStatus = WiFi.status;
 
 switch (_state) {
 case WifiState::CONNECTING:
 if (wifiStatus == WL_CONNECTED) {
 handleConnected;
 }
 else if (now - _connectionStartTime > WifiConfig::CONNECT_TIMEOUT_MS) {
 handleConnectionTimeout;
 }
 else if (wifiStatus == WL_CONNECT_FAILED || wifiStatus == WL_NO_SSID_AVAIL) {
 handleDisconnected;
 }
 break;
 
 case WifiState::DISCONNECTED:
 if (wifiStatus == WL_CONNECTED) {
 handleConnected;
 }
 break;
 
 case WifiState::CONNECTED:
 if (wifiStatus != WL_CONNECTED) {
 handleDisconnected;
 }
 break;
 
 case WifiState::WIFI_DISABLED:
 case WifiState::WIFI_ERROR:
 break;
 }
}


void WifiService::shutdown {
 ESP_LOGI(TAG, "SHUTTING DOWN WI-FI RADIO ");
 
 
 if (_state == WifiState::CONNECTED || _state == WifiState::CONNECTING) {
 ESP_LOGI(TAG, "Disconnecting from network...");
 WiFi.disconnect(true);
 }
 
 WiFi.mode(WIFI_OFF);
 
 if (_ssid.length > 0) {
 _state = WifiState::DISCONNECTED;
 } else {
 _state = WifiState::WIFI_DISABLED;
 }
 
 _retryCount = 0;
 
 if (_onConnectionChange) {
 _onConnectionChange(false);
 }
 
 ESP_LOGI(TAG, "Wi-Fi radio shutdown complete. State: %s", getStateName);
}


bool WifiService::setCredentials(const String& ssid, const String& password) {
 if (ssid.length == 0) {
 ESP_LOGE(TAG, "Cannot set empty SSID");
 return false;
 }
 if (ssid.length > WifiConfig::SSID_MAX_LENGTH) {
 ESP_LOGE(TAG, "SSID too long: %u > %u", ssid.length, WifiConfig::SSID_MAX_LENGTH);
 return false;
 }
 if (password.length > WifiConfig::PASS_MAX_LENGTH) {
 ESP_LOGE(TAG, "Password too long: %u > %u", password.length, WifiConfig::PASS_MAX_LENGTH);
 return false;
 }
 
 Preferences prefs;
 if (!prefs.begin(WifiConfig::NVS_NAMESPACE, false)) {
 ESP_LOGE(TAG, "Failed to open NVS for writing credentials");
 return false;
 }
 
 bool success = true;
 success &= prefs.putString(WifiConfig::NVS_KEY_SSID, ssid);
 success &= prefs.putString(WifiConfig::NVS_KEY_PASS, password);
 prefs.end;
 
 if (!success) {
 ESP_LOGE(TAG, "Failed to save credentials to NVS");
 return false;
 }
 
 _ssid = ssid;
 _password = password;
 _retryCount = 0;
 
 ESP_LOGI(TAG, "New credentials set for SSID: %s", _ssid.c_str);
 
 if (_state == WifiState::CONNECTED || _state == WifiState::CONNECTING) {
 WiFi.disconnect;
 }
 
 _state = WifiState::DISCONNECTED;
 connect;
 
 return true;
}

void WifiService::clearCredentials {
 ESP_LOGI(TAG, "Clearing Wi-Fi credentials...");
 
 Preferences prefs;
 if (prefs.begin(WifiConfig::NVS_NAMESPACE, false)) {
 prefs.clear;
 prefs.end;
 }
 
 _ssid = "";
 _password = "";
 _retryCount = 0;
 
 WiFi.disconnect(true);
 _state = WifiState::WIFI_DISABLED;
 
 if (_onConnectionChange) {
 _onConnectionChange(false);
 }
 
 ESP_LOGI(TAG, "Wi-Fi credentials cleared, state: DISABLED");
}

bool WifiService::hasCredentials const {
 return _ssid.length > 0;
}

bool WifiService::startConnection {
 
 if (!_initialized) {
 ESP_LOGW(TAG, "startConnection: Service not initialized");
 return false;
 }
 
 if (!hasCredentials) {
 ESP_LOGW(TAG, "startConnection: No credentials stored");
 return false;
 }
 
 if (_state == WifiState::CONNECTED) {
 ESP_LOGI(TAG, "startConnection: Already connected");
 return true;
 }
 
 if (_state == WifiState::CONNECTING) {
 ESP_LOGI(TAG, "startConnection: Connection already in progress");
 return true;
 }
 
 ESP_LOGI(TAG, "startConnection: Starting connection to SSID: %s", _ssid.c_str);
 _state = WifiState::DISCONNECTED;
 _retryCount = 0;
 connect;
 
 return true;
}


bool WifiService::isConnected const {
 return _state == WifiState::CONNECTED;
}

WifiState WifiService::getState const {
 return _state;
}

int16_t WifiService::getRSSI const {
 if (_state == WifiState::CONNECTED) {
 return WiFi.RSSI;
 }
 return 0;
}

String WifiService::getSSID const {
 return _ssid;
}

String WifiService::getIP const {
 if (_state == WifiState::CONNECTED) {
 return WiFi.localIP.toString;
 }
 return "0.0.0.0";
}

uint8_t WifiService::getRetryCount const {
 return _retryCount;
}


void WifiService::setConnectionCallback(ConnectionCallback callback) {
 _onConnectionChange = callback;
}

void WifiService::setStatusCallback(StatusCallback callback) {
 _onStatusChange = callback;
}


void WifiService::connect {
 if (_ssid.length == 0) {
 ESP_LOGW(TAG, "Cannot connect - no SSID configured");
 _state = WifiState::WIFI_DISABLED;
 return;
 }
 
 ESP_LOGI(TAG, "ENABLING WI-FI RADIO (One-Shot)");
 ESP_LOGI(TAG, " Target SSID: %s", _ssid.c_str);
 
 WiFi.mode(WIFI_STA);
 
 WiFi.setAutoReconnect(false);
 
 WiFi.setHostname("SensorDevice");
 
 esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
 
 _state = WifiState::CONNECTING;
 _connectionStartTime = millis;
 _lastConnectionAttempt = millis;
 _retryCount = 0;
 
 notifyStatus;
 
 WiFi.begin(_ssid.c_str, _password.c_str);
 
 ESP_LOGI(TAG, "Wi-Fi connection initiated (timeout: %lu ms)", WifiConfig::CONNECT_TIMEOUT_MS);
}

void WifiService::handleConnectionTimeout {
 ESP_LOGW(TAG, "CONNECTION TIMEOUT after %lu ms", WifiConfig::CONNECT_TIMEOUT_MS);
 
 wl_status_t status = WiFi.status;
 ESP_LOGW(TAG, " WiFi status: %d", status);
 
 
 WiFi.disconnect;
 
 _state = WifiState::WIFI_ERROR;
 _lastConnectionAttempt = millis;
 
 notifyStatus;
 
 if (_onConnectionChange) {
 _onConnectionChange(false);
 }
 
 ESP_LOGW(TAG, "Connection attempt failed. Radio still ON (waiting for policy decision).");
}

void WifiService::handleConnected {
 _state = WifiState::CONNECTED;
 _retryCount = 0;
 
 ESP_LOGI(TAG, "Wi-Fi CONNECTED!");
 ESP_LOGI(TAG, " SSID: %s", WiFi.SSID.c_str);
 ESP_LOGI(TAG, " IP: %s", WiFi.localIP.toString.c_str);
 ESP_LOGI(TAG, " RSSI: %d dBm", WiFi.RSSI);
 ESP_LOGI(TAG, " Gateway: %s", WiFi.gatewayIP.toString.c_str);
 
 notifyStatus;
 
 if (_onConnectionChange) {
 _onConnectionChange(true);
 }
}

void WifiService::handleDisconnected {
 uint8_t reason = _lastDisconnectReason;
 
 ESP_LOGW(TAG, "WI-FI DISCONNECTED");
 ESP_LOGW(TAG, " Reason Code: %d", reason);
 
 
 bool isAuthFailure = (reason == WIFI_REASON_AUTH_FAIL || 
 reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
 reason == 15);
 bool isApNotFound = (reason == WIFI_REASON_NO_AP_FOUND || reason == 201);
 
 if (isAuthFailure) {
 ESP_LOGW(TAG, " -> AUTH FAILURE (wrong password?)");
 } else if (isApNotFound) {
 ESP_LOGW(TAG, " -> NETWORK NOT FOUND (out of range?)");
 }
 
 _state = WifiState::WIFI_ERROR;
 _lastConnectionAttempt = millis;
 
 notifyStatus;
 
 if (_onConnectionChange) {
 _onConnectionChange(false);
 }
}

unsigned long WifiService::calculateRetryDelay const {
 if (_retryCount >= WifiConfig::MAX_RETRIES) {
 return WifiConfig::LONG_BACKOFF_MS;
 }
 return WifiConfig::RETRY_INTERVAL_MS;
}


void WifiService::getDiagnostics(char* buffer, size_t bufferSize) const {
 snprintf(buffer, bufferSize,
 "WiFi: state=%s, ssid=%s, ip=%s, rssi=%d, retries=%u",
 getStateName,
 _ssid.length > 0 ? _ssid.c_str : "(none)",
 getIP.c_str,
 isConnected ? getRSSI : 0,
 _retryCount);
}

const char* WifiService::getStateName const {
 switch (_state) {
 case WifiState::WIFI_DISABLED: return "DISABLED";
 case WifiState::DISCONNECTED: return "DISCONNECTED";
 case WifiState::CONNECTING: return "CONNECTING";
 case WifiState::CONNECTED: return "CONNECTED";
 case WifiState::WIFI_ERROR: return "ERROR";
 default: return "UNKNOWN";
 }
}


void WifiService::notifyStatus {
 if (_onStatusChange == nullptr) {
 return;
 }
 
 uint8_t state = static_cast<uint8_t>(_state);
 uint8_t reason = 0;
 int8_t rssi = 0;
 uint32_t ip = 0;
 
 if (_state == WifiState::CONNECTED) {
 rssi = WiFi.RSSI;
 ip = (uint32_t)WiFi.localIP;
 }
 
 if (_state == WifiState::WIFI_ERROR) {
 uint8_t wifiReason = _lastDisconnectReason;
 if (wifiReason == WIFI_REASON_AUTH_FAIL || 
 wifiReason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
 wifiReason == 15) {
 reason = 1;
 } else if (wifiReason == WIFI_REASON_NO_AP_FOUND || wifiReason == 201) {
 reason = 2;
 } else {
 reason = 255;
 }
 }
 
 ESP_LOGI(TAG, "Notifying status: state=%u, reason=%u, rssi=%d, ip=%lu", 
 state, reason, rssi, ip);
 
 _onStatusChange(state, reason, rssi, ip);
}

