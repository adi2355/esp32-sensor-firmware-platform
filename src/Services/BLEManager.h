
#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "../Config.h"
#include "HistoryService.h"

class BLEManager;


class BLEEventHandler {
public:
 virtual ~BLEEventHandler = default;
 
 virtual void onBLEConnect(uint16_t connHandle) = 0;
 virtual void onBLEDisconnect(uint16_t connHandle, int reason) = 0;
 virtual void onBLEBonded(bool success) = 0;
 
 virtual void onBLEDataReceived(const uint8_t* data, size_t length) = 0;
 
 virtual void onBLEAuthenticationComplete(bool authenticated) = 0;
 
 virtual void onBLESubscribe(bool subscribed) {}
};


class BLEServerCallbacksImpl : public NimBLEServerCallbacks {
private:
 BLEManager* _manager;
 
public:
 explicit BLEServerCallbacksImpl(BLEManager* manager);
 
 void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override;
 void onDisconnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override;
 
 uint32_t onPassKeyRequest override;
 void onAuthenticationComplete(ble_gap_conn_desc* desc) override;
 bool onConfirmPIN(uint32_t pin) override;
};


class BLECharCallbacksImpl : public NimBLECharacteristicCallbacks {
private:
 BLEManager* _manager;
 
public:
 explicit BLECharCallbacksImpl(BLEManager* manager);
 
 void onWrite(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc) override;
 void onRead(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc) override;
 void onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc, uint16_t subValue) override;
};


class BLEManager {
private:
 static BLEManager* _instance;
 
 NimBLEServer* _pServer;
 NimBLEService* _pMainService;
 NimBLEService* _pGattService;
 NimBLECharacteristic* _pMainChar;
 NimBLECharacteristic* _pOtaChar;
 NimBLECharacteristic* _pServiceChangedChar;
 NimBLECharacteristic* _pDatabaseHashChar;
 NimBLEAdvertising* _pAdvertising;
 
 BLEServerCallbacksImpl _serverCallbacks;
 BLECharCallbacksImpl _mainCharCallbacks;
 BLECharCallbacksImpl _otaCharCallbacks;
 
 BLEEventHandler* _eventHandler;
 
 HistoryService* _historyService;
 
 bool _initialized;
 volatile bool _deviceConnected;
 volatile bool _pairingModeEnabled;
 volatile bool _subscribed;
 
 volatile uint16_t _currentConnHandle;
 volatile unsigned long _connectionStartTime;
 unsigned long _pairingModeStartTime;
 volatile bool _securityRequested;
 
 volatile bool _paramsNeedUpdate;
 volatile bool _paramsUpdated;
 
 volatile bool _bootPairingWindowActive;
 unsigned long _bootTime;
 
 portMUX_TYPE _mux;
 
 BLEManager;
 
 BLEManager(const BLEManager&) = delete;
 BLEManager& operator=(const BLEManager&) = delete;
 
 friend class BLEServerCallbacksImpl;
 friend class BLECharCallbacksImpl;
 
public:
 static BLEManager* getInstance;
 
 bool init(HistoryService* historyService);
 
 void setEventHandler(BLEEventHandler* handler);
 
 
 bool isConnected const;
 
 uint16_t getConnectionHandle const;
 
 void disconnect;
 
 unsigned long getConnectionDuration const;
 
 
 void startAdvertising;
 
 void stopAdvertising;
 
 bool isAdvertising const;
 
 
 void enablePairingMode;
 
 void disablePairingMode;
 
 bool isPairingModeEnabled const;
 
 bool isPairingAllowed const;
 
 void deleteAllBonds;
 
 int getBondCount const;
 
 
 void incrementGhostBondCounter;
 
 void resetGhostBondCounter;
 
 
 bool sendNotification(const uint8_t* data, size_t length);
 
 bool sendIndication(const uint8_t* data, size_t length);
 
 bool isNotifySubscribed const;
 
 
 void indicateServiceChanged;
 
 void updateDatabaseHash;
 
 
 void update;
 
 void processDataQueue;
 
 
 void getDiagnostics(char* buffer, size_t bufferSize) const;
 
private:
 void configureSecurity;
 
 void createServices;
 
 void configureAdvertising;
 
 void handleMainCharWrite(const uint8_t* data, size_t length);
 
 void handleOtaCharWrite(const uint8_t* data, size_t length);
 
 void onConnectInternal(ble_gap_conn_desc* desc);
 
 void onDisconnectInternal(ble_gap_conn_desc* desc, int reason);
 
 void updateConnectionParams(uint16_t connHandle);
};


inline BLEManager* BLEManager::getInstance {
 static BLEManager instance;
 _instance = &instance;
 return _instance;
}

inline bool BLEManager::isConnected const {
 return _deviceConnected;
}

inline uint16_t BLEManager::getConnectionHandle const {
 return _deviceConnected ? _currentConnHandle : 0xFFFF;
}

inline bool BLEManager::isPairingModeEnabled const {
 return _pairingModeEnabled;
}

inline bool BLEManager::isNotifySubscribed const {
 return _subscribed;
}

#endif

