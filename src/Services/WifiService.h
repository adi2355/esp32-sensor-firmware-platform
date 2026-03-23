
#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "../Config.h"

namespace WifiConfig {
 constexpr unsigned long CONNECT_TIMEOUT_MS = 15000;
 
 constexpr unsigned long RETRY_INTERVAL_MS = 60000;
 
 constexpr uint8_t MAX_RETRIES = 5;
 
 constexpr unsigned long LONG_BACKOFF_MS = 300000;
 
 constexpr const char* NVS_NAMESPACE = "wifi_creds";
 constexpr const char* NVS_KEY_SSID = "ssid";
 constexpr const char* NVS_KEY_PASS = "pass";
 
 constexpr size_t SSID_MAX_LENGTH = 32;
 constexpr size_t PASS_MAX_LENGTH = 64;
}

enum class WifiState : uint8_t {
 WIFI_DISABLED,
 DISCONNECTED,
 CONNECTING,
 CONNECTED,
 WIFI_ERROR
};

class WifiService {
public:
 
 static WifiService* getInstance;
 
 
 void init;
 
 void update;
 
 void shutdown;
 
 
 bool setCredentials(const String& ssid, const String& password);
 
 void clearCredentials;
 
 bool hasCredentials const;
 
 bool startConnection;
 
 
 bool isConnected const;
 
 WifiState getState const;
 
 int16_t getRSSI const;
 
 String getSSID const;
 
 String getIP const;
 
 uint8_t getRetryCount const;
 
 
 using ConnectionCallback = void (*)(bool connected);
 
 void setConnectionCallback(ConnectionCallback callback);
 
 using StatusCallback = void (*)(uint8_t state, uint8_t reason, int8_t rssi, uint32_t ip);
 
 void setStatusCallback(StatusCallback callback);
 
 
 void getDiagnostics(char* buffer, size_t bufferSize) const;
 
 const char* getStateName const;

private:
 WifiService;
 
 WifiService(const WifiService&) = delete;
 WifiService& operator=(const WifiService&) = delete;
 
 static WifiService* _instance;
 
 
 WifiState _state;
 String _ssid;
 String _password;
 
 unsigned long _lastConnectionAttempt;
 unsigned long _connectionStartTime;
 uint8_t _retryCount;
 bool _initialized;
 
 uint8_t _lastDisconnectReason;
 
 ConnectionCallback _onConnectionChange;
 StatusCallback _onStatusChange;
 
 
 void connect;
 
 void handleConnectionTimeout;
 
 void handleConnected;
 
 void handleDisconnected;
 
 unsigned long calculateRetryDelay const;
 
 void notifyStatus;
};

#endif

