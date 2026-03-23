
#include "FlasherService.h"
#include "../Config.h"
#include "../HAL/LED_HAL.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

static const char* TAG = "FLASHER";


enum class FlasherAction : uint8_t {
 NONE = 0,
 ABORT_RESET,
 START_SESSION,
 REBOOT_SUCCESS
};

static volatile FlasherAction g_pendingAction = FlasherAction::NONE;

static volatile uint32_t g_pendingSessionId = 0;
static volatile uint32_t g_pendingTotalSize = 0;


static WebServer* g_server = nullptr;


struct FlasherState {
 FlasherPhase phase;
 
 uint32_t sessionId;
 uint32_t written;
 uint32_t total;
 int32_t lastChunkIndex;
 
 unsigned long lastActivityMs;
 unsigned long lastProgressMs;
 
 uint16_t httpCode;
 uint8_t error;
 char message[64];
 
 uint32_t vIdle;
 uint32_t vLoad;
 
 bool uploadStarted;
};

static FlasherState g_state = {
 FlasherPhase::INIT,
 0,
 0,
 0,
 -1,
 0,
 0,
 200,
 FlasherConfig::Error::NONE,
 {},
 0,
 0,
 false
};


enum class ChunkBufferState : uint8_t {
 IDLE = 0,
 RECEIVING,
 QUEUED,
 WRITING,
 ERROR
};

struct PendingChunk {
 ChunkBufferState state;
 uint32_t sessionId;
 int32_t chunkIndex;
 int32_t totalChunks;
 size_t expectedSize;
 size_t receivedSize;
 unsigned long startMs;
 uint8_t data[FlasherConfig::MAX_CHUNK_SIZE];
};

static PendingChunk g_pendingChunk = {
 ChunkBufferState::IDLE,
 0,
 -1,
 0,
 0,
 0,
 0,
 {}
};

static int g_chunkResponseCode = 200;
static char g_chunkResponseMsg[64] = "Chunk queued";


const char* flasherPhaseToString(FlasherPhase phase) {
 switch (phase) {
 case FlasherPhase::INIT: return "INIT";
 case FlasherPhase::BATTERY_CHECK: return "BATTERY_CHECK";
 case FlasherPhase::WIFI_START: return "WIFI_START";
 case FlasherPhase::READY: return "READY";
 case FlasherPhase::ERASING: return "ERASING";
 case FlasherPhase::VERIFYING: return "VERIFYING";
 case FlasherPhase::COMPLETE: return "COMPLETE";
 case FlasherPhase::SUCCESS: return "SUCCESS";
 case FlasherPhase::ERROR: return "ERROR";
 default: return "UNKNOWN";
 }
}

const char* chunkBufferStateToString(ChunkBufferState state) {
 switch (state) {
 case ChunkBufferState::IDLE: return "IDLE";
 case ChunkBufferState::RECEIVING: return "RECEIVING";
 case ChunkBufferState::QUEUED: return "QUEUED";
 case ChunkBufferState::WRITING: return "WRITING";
 case ChunkBufferState::ERROR: return "ERROR";
 default: return "UNKNOWN";
 }
}

static void setChunkResponse(int httpCode, const char* message) {
 g_chunkResponseCode = httpCode;
 strncpy(g_chunkResponseMsg, message, sizeof(g_chunkResponseMsg) - 1);
 g_chunkResponseMsg[sizeof(g_chunkResponseMsg) - 1] = '\0';
}

static void resetPendingChunk {
 g_pendingChunk.state = ChunkBufferState::IDLE;
 g_pendingChunk.sessionId = 0;
 g_pendingChunk.chunkIndex = -1;
 g_pendingChunk.totalChunks = 0;
 g_pendingChunk.expectedSize = 0;
 g_pendingChunk.receivedSize = 0;
 g_pendingChunk.startMs = 0;
}

static bool isTransitionAllowed(FlasherPhase from, FlasherPhase to) {
 if (to == FlasherPhase::ERROR) {
 return true;
 }

 switch (from) {
 case FlasherPhase::INIT:
 return to == FlasherPhase::BATTERY_CHECK;
 case FlasherPhase::BATTERY_CHECK:
 return to == FlasherPhase::WIFI_START;
 case FlasherPhase::WIFI_START:
 return to == FlasherPhase::READY;
 case FlasherPhase::READY:
 return to == FlasherPhase::ERASING;
 case FlasherPhase::ERASING:
 return to == FlasherPhase::RECEIVING;
 case FlasherPhase::RECEIVING:
 return to == FlasherPhase::VERIFYING || to == FlasherPhase::COMPLETE;
 case FlasherPhase::VERIFYING:
 return to == FlasherPhase::COMPLETE;
 case FlasherPhase::COMPLETE:
 case FlasherPhase::SUCCESS:
 return false;
 case FlasherPhase::ERROR:
 default:
 return false;
 }
}

static void transitionPhase(FlasherPhase next, const char* reason) {
 if (g_state.phase == next) {
 return;
 }

 if (!isTransitionAllowed(g_state.phase, next)) {
 ESP_LOGE(TAG, "Invalid phase transition: %s -> %s (%s)",
 flasherPhaseToString(g_state.phase),
 flasherPhaseToString(next),
 reason ? reason : "no reason");
 return;
 }

 ESP_LOGI(TAG, "Phase transition: %s -> %s (%s)",
 flasherPhaseToString(g_state.phase),
 flasherPhaseToString(next),
 reason ? reason : "no reason");
 g_state.phase = next;
}

static void forcePhase(FlasherPhase next, const char* reason) {
 if (g_state.phase != next) {
 ESP_LOGI(TAG, "Phase force-set: %s -> %s (%s)",
 flasherPhaseToString(g_state.phase),
 flasherPhaseToString(next),
 reason ? reason : "no reason");
 }
 g_state.phase = next;
}

static void setErrorState(uint8_t errorCode, const char* message, int httpCode) {
 g_state.httpCode = httpCode;
 g_state.error = errorCode;
 strncpy(g_state.message, message, sizeof(g_state.message) - 1);
 g_state.message[sizeof(g_state.message) - 1] = '\0';
 g_state.phase = FlasherPhase::ERROR;
}

static void processPendingChunk {
 if (g_pendingChunk.state != ChunkBufferState::QUEUED) {
 return;
 }

 if (g_state.phase != FlasherPhase::RECEIVING) {
 resetPendingChunk;
 return;
 }

 g_pendingChunk.state = ChunkBufferState::WRITING;
 esp_task_wdt_reset;

 size_t bytesWritten = Update.write(g_pendingChunk.data, g_pendingChunk.receivedSize);

 esp_task_wdt_reset;

 if (bytesWritten != g_pendingChunk.receivedSize) {
 uint8_t updateErr = Update.getError;
 ESP_LOGE(TAG, "Flash write failed: %zu/%zu (err=%d)",
 bytesWritten, g_pendingChunk.receivedSize, updateErr);
 setErrorState(FlasherConfig::Error::FLASH_WRITE_FAILED, "Flash write failed", 500);
 Update.abort;
 g_pendingChunk.state = ChunkBufferState::ERROR;
 return;
 }

 g_state.written += bytesWritten;
 g_state.lastChunkIndex = g_pendingChunk.chunkIndex;
 g_state.lastProgressMs = millis;

 if (g_pendingChunk.totalChunks > 0 &&
 (g_pendingChunk.chunkIndex % 10 == 0 ||
 g_pendingChunk.chunkIndex == g_pendingChunk.totalChunks - 1)) {
 uint8_t pct = (g_state.total > 0) ? (g_state.written * 100 / g_state.total) : 0;
 ESP_LOGI(TAG, "Chunk %ld/%ld committed. Total: %lu bytes (%u%%)",
 g_pendingChunk.chunkIndex + 1,
 g_pendingChunk.totalChunks,
 g_state.written,
 pct);
 }

 if (g_pendingChunk.totalChunks > 0 &&
 g_pendingChunk.chunkIndex == g_pendingChunk.totalChunks - 1) {
 transitionPhase(FlasherPhase::VERIFYING, "final chunk");
 esp_task_wdt_reset;

 if (Update.end(true)) {
 esp_task_wdt_reset;
 strncpy(g_state.message, "Success. Rebooting.", sizeof(g_state.message));
 transitionPhase(FlasherPhase::COMPLETE, "verify ok");
 g_pendingAction = FlasherAction::REBOOT_SUCCESS;
 } else {
 esp_task_wdt_reset;
 ESP_LOGE(TAG, "Update.end failed: %d", Update.getError);
 setErrorState(FlasherConfig::Error::FLASH_END_FAILED, "Verify failed", 500);
 }
 }

 if (g_pendingChunk.state != ChunkBufferState::ERROR) {
 resetPendingChunk;
 }
}
static void resetFlasherState {
 esp_task_wdt_reset;
 
 if (Update.isRunning) {
 ESP_LOGW(TAG, "Aborting in-progress Update...");
 Update.abort;
 }
 
 Update.clearError;
 
 forcePhase(FlasherPhase::READY, "reset");
 g_state.sessionId = 0;
 g_state.written = 0;
 g_state.total = 0;
 g_state.lastChunkIndex = -1;
 g_state.lastActivityMs = millis;
 g_state.lastProgressMs = 0;
 g_state.httpCode = 200;
 g_state.error = FlasherConfig::Error::NONE;
 strncpy(g_state.message, "Ready", sizeof(g_state.message));
 g_state.uploadStarted = false;

 resetPendingChunk;
 setChunkResponse(200, "Ready");
 
 g_pendingAction = FlasherAction::NONE;
 g_pendingSessionId = 0;
 g_pendingTotalSize = 0;
 
 ESP_LOGI(TAG, "State reset complete. Ready for new session.");
}

static uint32_t readBatteryVoltageInternal {
 uint32_t adcMv = analogReadMilliVolts(Pins::BAT_ADC);
 
 uint32_t batteryMv = (uint32_t)(adcMv * FlasherConfig::DIVIDER_RATIO);
 
 if (batteryMv < 1500) {
 ESP_LOGW(TAG, "Low voltage detected (%lumV) - assuming USB power", batteryMv);
 return 4200;
 }
 
 return batteryMv;
}

uint32_t FlasherService::readBatteryVoltage {
 return readBatteryVoltageInternal;
}

void FlasherService::setLedColor(uint32_t color) {
 LED_HAL* led = LED_HAL::getInstance;
 if (led) {
 led->setColor(color);
 }
}

void FlasherService::flashLed(uint8_t count, uint32_t color) {
 LED_HAL* led = LED_HAL::getInstance;
 if (!led) return;
 
 for (uint8_t i = 0; i < count; i++) {
 led->setColor(color);
 delay(100);
 led->off;
 delay(100);
 }
}

void FlasherService::errorAndReboot(uint8_t errorCode, const char* message) {
 ESP_LOGE(TAG, "ERROR [%d]: %s", errorCode, message);
 
 g_state.phase = FlasherPhase::ERROR;
 g_state.error = errorCode;
 strncpy(g_state.message, message, sizeof(g_state.message) - 1);
 g_state.message[sizeof(g_state.message) - 1] = '\0';
 
 uint8_t flashCount = (errorCode > 0 && errorCode <= 10) ? errorCode : 5;
 flashLed(flashCount, 0xFF0000);
 
 delay(1000);
 ESP_LOGI(TAG, "Rebooting...");
 Serial.flush;
 ESP.restart;
}


static void setupServer {
 ESP_LOGI(TAG, "Setting up HTTP server (- Explicit Erase)...");
 
 if (g_server != nullptr) {
 ESP_LOGW(TAG, "WebServer already initialized - skipping setup");
 return;
 }
 
 g_server = new WebServer(FlasherConfig::HTTP_PORT);
 if (g_server == nullptr) {
 ESP_LOGE(TAG, "FATAL: Failed to allocate WebServer");
 ESP.restart;
 return;
 }
 
 const char* headerKeys[] = {
 "Content-Length", 
 "X-Session-ID", 
 "X-Chunk-Index", 
 "X-Total-Chunks",
 "X-Total-Size",
 "X-Chunk-Size"
 };
 g_server->collectHeaders(headerKeys, 6);
 
 g_server->on("/status", HTTP_GET, [] {
 if (g_server == nullptr) return;
 
 char json[512];
 uint32_t currentBatt = readBatteryVoltageInternal;
 uint32_t idleSec = (millis - g_state.lastActivityMs) / 1000;
 
 snprintf(json, sizeof(json),
 "{\"session\":%lu,\"phase\":\"%s\",\"written\":%lu,\"total\":%lu,"
 "\"code\":%d,\"err\":%d,\"msg\":\"%s\","
 "\"v_batt\":%lu,\"v_idle\":%lu,\"v_load\":%lu,\"idle\":%lu,"
 "\"lastChunk\":%ld,\"chunkState\":\"%s\",\"pendingChunk\":%ld,\"pendingSize\":%lu}",
 g_state.sessionId,
 flasherPhaseToString(g_state.phase),
 g_state.written,
 g_state.total,
 g_state.httpCode,
 g_state.error,
 g_state.message,
 currentBatt,
 g_state.vIdle,
 g_state.vLoad,
 idleSec,
 g_state.lastChunkIndex,
 chunkBufferStateToString(g_pendingChunk.state),
 g_pendingChunk.chunkIndex,
 static_cast<unsigned long>(g_pendingChunk.receivedSize)
 );
 
 g_server->send(200, "application/json", json);
 g_state.lastActivityMs = millis;
 
 ESP_LOGD(TAG, "/status response sent (phase=%s, chunk=%ld)", 
 flasherPhaseToString(g_state.phase), g_state.lastChunkIndex);
 });
 
 g_server->on("/start", HTTP_POST, [] {
 if (g_server == nullptr) return;
 
 g_state.lastActivityMs = millis;
 
 ESP_LOGI(TAG, "/start request received");
 
 if (g_state.phase != FlasherPhase::READY) {
 ESP_LOGW(TAG, "/start rejected: not in READY state (phase=%s)", 
 flasherPhaseToString(g_state.phase));
 
 if (g_state.phase == FlasherPhase::ERASING) {
 g_server->send(409, "text/plain", "Erase in progress - poll /status");
 } else if (g_state.phase == FlasherPhase::RECEIVING) {
 g_server->send(409, "text/plain", "Session active - send chunks or /abort");
 } else {
 g_server->send(409, "text/plain", "Device busy");
 }
 return;
 }
 
 String sessionStr = g_server->header("X-Session-ID");
 String totalSizeStr = g_server->header("X-Total-Size");
 
 ESP_LOGI(TAG, "/start headers: session=%s, totalSize=%s",
 sessionStr.c_str, totalSizeStr.c_str);
 
 if (sessionStr.isEmpty || totalSizeStr.isEmpty) {
 ESP_LOGE(TAG, "/start missing headers");
 g_server->send(400, "text/plain", "Missing X-Session-ID or X-Total-Size headers");
 return;
 }
 
 uint32_t reqSessionId = strtoul(sessionStr.c_str, NULL, 10);
 uint32_t reqTotalSize = strtoul(totalSizeStr.c_str, NULL, 10);
 
 if (reqSessionId == 0) {
 ESP_LOGE(TAG, "/start invalid session ID: 0");
 g_server->send(400, "text/plain", "Invalid X-Session-ID (cannot be 0)");
 return;
 }
 
 if (reqTotalSize < 1024) {
 ESP_LOGE(TAG, "/start total size too small: %lu", reqTotalSize);
 g_server->send(400, "text/plain", "X-Total-Size too small (< 1KB)");
 return;
 }
 
 const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
 if (partition == NULL) {
 ESP_LOGE(TAG, "/start no OTA partition available");
 g_server->send(500, "text/plain", "No OTA partition available");
 return;
 }
 
 if (reqTotalSize > partition->size) {
 ESP_LOGE(TAG, "/start firmware too large: %lu > %lu", reqTotalSize, partition->size);
 char errMsg[64];
 snprintf(errMsg, sizeof(errMsg), "Firmware too large (%lu > %lu bytes)", 
 reqTotalSize, partition->size);
 g_server->send(413, "text/plain", errMsg);
 return;
 }
 
 g_pendingSessionId = reqSessionId;
 g_pendingTotalSize = reqTotalSize;
 g_pendingAction = FlasherAction::START_SESSION;
 
 ESP_LOGI(TAG, "/start accepted: session=%lu, size=%lu, partition=%s",
 reqSessionId, reqTotalSize, partition->label);
 
 g_server->send(200, "text/plain", "Session started - erasing flash...");
 
 ESP_LOGI(TAG, "/start response sent - START_SESSION action pending");
 });
 
 g_server->on("/abort", HTTP_POST, [] {
 if (g_server == nullptr) return;
 
 ESP_LOGW(TAG, "/abort requested - flagging for deferred reset");
 
 g_pendingAction = FlasherAction::ABORT_RESET;
 
 g_server->send(200, "text/plain", "ABORTED");
 
 ESP_LOGI(TAG, "/abort response sent - reset pending in main loop");
 });
 
 g_server->on("/", HTTP_GET, [] {
 if (g_server == nullptr) return;
 
#ifdef DIST_BUILD
 g_server->send(200, "text/plain", "OTA Ready");
#else
 g_server->send(200, "text/plain",
 "OTA Server (Explicit Erase)\n"
 "1. POST /start with X-Session-ID, X-Total-Size headers\n"
 "2. Poll /status until phase=RECEIVING\n"
 "3. POST chunks to /update_chunk\n"
 "POST /abort to cancel\n\n"
 "Chunk Headers: X-Session-ID, X-Chunk-Index, X-Total-Chunks");
#endif
 });
 
 g_server->on("/update_chunk", HTTP_POST,
 [] {
 if (g_server == nullptr) return;

 esp_task_wdt_reset;
 g_state.lastActivityMs = millis;

 if (g_state.phase == FlasherPhase::ERROR) {
 int httpCode = (g_state.httpCode >= 400) ? g_state.httpCode : 500;
 g_server->send(httpCode, "text/plain", g_state.message);
 return;
 }

 g_server->send(g_chunkResponseCode, "text/plain", g_chunkResponseMsg);
 },
 [] {
 if (g_server == nullptr) return;

 HTTPUpload& upload = g_server->upload;
 esp_task_wdt_reset;

 if (upload.status == UPLOAD_FILE_START) {
 g_state.lastActivityMs = millis;
 setChunkResponse(202, "Chunk queued");

 if (g_state.phase != FlasherPhase::RECEIVING) {
 setChunkResponse(409, "Not ready for chunks");
 return;
 }

 if (g_pendingChunk.state != ChunkBufferState::IDLE) {
 setChunkResponse(409, "Chunk buffer busy");
 return;
 }

 String sessionStr = g_server->header("X-Session-ID");
 String chunkStr = g_server->header("X-Chunk-Index");
 String totalStr = g_server->header("X-Total-Chunks");
 String lengthStr = g_server->header("Content-Length");
 String chunkSizeStr = g_server->header("X-Chunk-Size");

 if (sessionStr.isEmpty || chunkStr.isEmpty || totalStr.isEmpty) {
 setErrorState(FlasherConfig::Error::NO_CONTENT_LENGTH,
 "Missing headers", 400);
 setChunkResponse(400, "Missing headers");
 g_pendingChunk.state = ChunkBufferState::ERROR;
 return;
 }

 uint32_t reqSessionId = strtoul(sessionStr.c_str, NULL, 10);
 int32_t chunkIndex = atoi(chunkStr.c_str);
 int32_t totalChunks = atoi(totalStr.c_str);

 if (g_state.sessionId != reqSessionId) {
 setErrorState(FlasherConfig::Error::SESSION_MISMATCH,
 "Session mismatch", 409);
 setChunkResponse(409, "Session mismatch");
 g_pendingChunk.state = ChunkBufferState::ERROR;
 return;
 }

 if (chunkIndex <= g_state.lastChunkIndex) {
 setChunkResponse(200, "Duplicate chunk");
 return;
 }

 size_t expectedSize = 0;
 if (!chunkSizeStr.isEmpty) {
 expectedSize = static_cast<size_t>(chunkSizeStr.toInt);
 } else if (!lengthStr.isEmpty) {
 expectedSize = static_cast<size_t>(lengthStr.toInt);
 }

 if (expectedSize == 0 || expectedSize > FlasherConfig::MAX_CHUNK_SIZE) {
 setErrorState(FlasherConfig::Error::INVALID_CHUNK,
 "Invalid chunk size", 413);
 setChunkResponse(413, "Invalid chunk size");
 g_pendingChunk.state = ChunkBufferState::ERROR;
 return;
 }

 g_pendingChunk.state = ChunkBufferState::RECEIVING;
 g_pendingChunk.sessionId = reqSessionId;
 g_pendingChunk.chunkIndex = chunkIndex;
 g_pendingChunk.totalChunks = totalChunks;
 g_pendingChunk.expectedSize = expectedSize;
 g_pendingChunk.receivedSize = 0;
 g_pendingChunk.startMs = millis;

 } else if (upload.status == UPLOAD_FILE_WRITE) {
 if (g_pendingChunk.state != ChunkBufferState::RECEIVING) return;

 if (upload.buf == nullptr && upload.currentSize > 0) {
 setErrorState(FlasherConfig::Error::INVALID_CHUNK,
 "Null upload buffer", 500);
 setChunkResponse(500, "Null upload buffer");
 g_pendingChunk.state = ChunkBufferState::ERROR;
 return;
 }

 if (upload.currentSize == 0) return;

 size_t nextSize = g_pendingChunk.receivedSize + upload.currentSize;
 if (nextSize > g_pendingChunk.expectedSize ||
 nextSize > FlasherConfig::MAX_CHUNK_SIZE) {
 setErrorState(FlasherConfig::Error::INVALID_CHUNK,
 "Chunk overflow", 400);
 setChunkResponse(400, "Chunk overflow");
 g_pendingChunk.state = ChunkBufferState::ERROR;
 return;
 }

 memcpy(&g_pendingChunk.data[g_pendingChunk.receivedSize],
 upload.buf,
 upload.currentSize);
 g_pendingChunk.receivedSize = nextSize;

 } else if (upload.status == UPLOAD_FILE_END) {
 if (g_pendingChunk.state != ChunkBufferState::RECEIVING) return;

 if (g_pendingChunk.receivedSize != g_pendingChunk.expectedSize) {
 setErrorState(FlasherConfig::Error::INVALID_CHUNK,
 "Chunk size mismatch", 400);
 setChunkResponse(400, "Chunk size mismatch");
 g_pendingChunk.state = ChunkBufferState::ERROR;
 return;
 }

 g_pendingChunk.state = ChunkBufferState::QUEUED;
 g_pendingChunk.startMs = millis;
 strncpy(g_state.message, "Chunk queued", sizeof(g_state.message));

 } else if (upload.status == UPLOAD_FILE_ABORTED) {
 setChunkResponse(499, "Client aborted");
 resetPendingChunk;
 }
 }
 );
 
 g_server->on("/update", HTTP_POST, 
 [] {
 if (g_server == nullptr) return;
 
 LED_HAL* led = LED_HAL::getInstance;
 
 if (Update.hasError || g_state.phase == FlasherPhase::ERROR) {
 int httpCode = (g_state.httpCode >= 400) ? g_state.httpCode : 500;
 ESP_LOGE(TAG, "[LEGACY] Update ERROR: code=%d, msg=%s", httpCode, g_state.message);
 g_server->send(httpCode, "text/plain", g_state.message);
 if (led) led->setColor(0xFF0000);
 } else {
 ESP_LOGI(TAG, "[LEGACY] Update SUCCESS - flagging deferred reboot");
 g_state.phase = FlasherPhase::COMPLETE;
 strncpy(g_state.message, "Success. Rebooting.", sizeof(g_state.message));
 g_pendingAction = FlasherAction::REBOOT_SUCCESS;
 g_server->send(200, "text/plain", "OK - Rebooting...");
 if (led) led->setColor(0x00FF00);
 }
 },
 [] {
 if (g_server == nullptr) return;
 
 HTTPUpload& upload = g_server->upload;
 LED_HAL* led = LED_HAL::getInstance;
 
 esp_task_wdt_reset;
 g_state.lastActivityMs = millis;
 
 if (upload.status == UPLOAD_FILE_START) {
 ESP_LOGI(TAG, "[LEGACY] Streaming upload started: %s", upload.filename.c_str);
 
 if (g_state.phase != FlasherPhase::READY) {
 if (Update.isRunning) {
 Update.abort;
 }
 g_state.written = 0;
 g_state.total = 0;
 g_state.error = FlasherConfig::Error::NONE;
 g_state.httpCode = 200;
 }
 
 g_state.phase = FlasherPhase::RECEIVING;
 if (led) led->setColor(0x0000FF);
 
 if (!g_server->hasHeader("Content-Length")) {
 g_state.httpCode = 411;
 g_state.error = FlasherConfig::Error::NO_CONTENT_LENGTH;
 strncpy(g_state.message, "Missing Content-Length", sizeof(g_state.message));
 g_state.phase = FlasherPhase::ERROR;
 return;
 }
 
 g_state.total = g_server->header("Content-Length").toInt;
 
 const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
 if (partition == NULL || g_state.total > partition->size) {
 g_state.httpCode = 413;
 g_state.error = FlasherConfig::Error::SIZE_TOO_LARGE;
 strncpy(g_state.message, "Firmware too large", sizeof(g_state.message));
 g_state.phase = FlasherPhase::ERROR;
 return;
 }
 
 esp_task_wdt_reset;
 
 if (!Update.begin(g_state.total, U_FLASH)) {
 g_state.httpCode = 500;
 g_state.error = FlasherConfig::Error::FLASH_BEGIN_FAILED;
 snprintf(g_state.message, sizeof(g_state.message), "Begin failed: %d", Update.getError);
 g_state.phase = FlasherPhase::ERROR;
 return;
 }
 
 esp_task_wdt_reset;
 
 g_state.written = 0;
 g_state.lastProgressMs = millis;
 g_state.uploadStarted = true;
 g_state.sessionId = millis;
 
 } else if (upload.status == UPLOAD_FILE_WRITE) {
 if (g_state.phase == FlasherPhase::ERROR) return;
 if (upload.currentSize == 0) return;
 
 esp_task_wdt_reset;
 
 size_t bytesWritten = Update.write(upload.buf, upload.currentSize);
 
 esp_task_wdt_reset;
 if (bytesWritten != upload.currentSize) {
 g_state.httpCode = 500;
 g_state.error = FlasherConfig::Error::FLASH_WRITE_FAILED;
 snprintf(g_state.message, sizeof(g_state.message), "Write failed: %d", Update.getError);
 g_state.phase = FlasherPhase::ERROR;
 Update.abort;
 return;
 }
 
 g_state.written += bytesWritten;
 g_state.lastProgressMs = millis;
 
 } else if (upload.status == UPLOAD_FILE_END) {
 if (g_state.phase == FlasherPhase::ERROR) return;
 
 g_state.phase = FlasherPhase::VERIFYING;
 
 if (g_state.written != g_state.total) {
 g_state.httpCode = 400;
 g_state.error = FlasherConfig::Error::SIZE_TOO_LARGE;
 snprintf(g_state.message, sizeof(g_state.message), "Incomplete: %lu/%lu", g_state.written, g_state.total);
 g_state.phase = FlasherPhase::ERROR;
 Update.abort;
 return;
 }
 
 esp_task_wdt_reset;
 
 if (!Update.end(true)) {
 esp_task_wdt_reset;
 g_state.httpCode = 500;
 g_state.error = FlasherConfig::Error::FLASH_END_FAILED;
 snprintf(g_state.message, sizeof(g_state.message), "Commit failed: %d", Update.getError);
 g_state.phase = FlasherPhase::ERROR;
 return;
 }
 
 esp_task_wdt_reset;
 
 strncpy(g_state.message, "Verified", sizeof(g_state.message));
 
 } else if (upload.status == UPLOAD_FILE_ABORTED) {
 g_state.httpCode = 499;
 g_state.error = FlasherConfig::Error::USER_ABORT;
 strncpy(g_state.message, "Client aborted", sizeof(g_state.message));
 g_state.phase = FlasherPhase::ERROR;
 Update.abort;
 }
 }
 );
 
 g_server->onNotFound([] {
 if (g_server == nullptr) return;
 
 ESP_LOGW(TAG, "404: %s %s", 
 (g_server->method == HTTP_GET) ? "GET" : "POST",
 g_server->uri.c_str);
 
 g_server->send(404, "text/plain", "Not Found");
 });
 
 g_server->begin;
 ESP_LOGI(TAG, "HTTP server started on port %d", FlasherConfig::HTTP_PORT);
}


void FlasherService::run {
#ifndef DIST_BUILD
 Serial.begin(115200);
 delay(200);
#endif
 
 ESP_LOGI(TAG, "FLASHER MODE - Explicit Erase OTA");
 ESP_LOGI(TAG, "FIX: Decoupled flash erase from HTTP handler");
 
 forcePhase(FlasherPhase::INIT, "startup");
 g_pendingAction = FlasherAction::NONE;
 g_pendingSessionId = 0;
 g_pendingTotalSize = 0;
 
 esp_task_wdt_init(30, true);
 esp_task_wdt_add(NULL);
 
 setCpuFrequencyMhz(240);
 ESP_LOGI(TAG, "CPU frequency set to 240MHz for OTA stability");
 
 LED_HAL* led = LED_HAL::getInstance;
 if (led) {
 led->init;
 led->setColor(0xFFA500);
 }
 
 pinMode(Pins::BAT_ADC, INPUT);
 
 g_state.lastActivityMs = millis;
 
 transitionPhase(FlasherPhase::BATTERY_CHECK, "battery check");
 ESP_LOGI(TAG, "Stage 1: Battery check at idle...");
 
 g_state.vIdle = readBatteryVoltage;
 
 if (g_state.vIdle < FlasherConfig::MIN_VOLTAGE_IDLE_MV) {
 char msg[64];
 snprintf(msg, sizeof(msg), "Battery too low at idle: %lumV < %lumV", 
 g_state.vIdle, FlasherConfig::MIN_VOLTAGE_IDLE_MV);
 errorAndReboot(FlasherConfig::Error::LOW_BATTERY_IDLE, msg);
 }
 
 ESP_LOGI(TAG, "Stage 1 PASS: %lumV >= %lumV", g_state.vIdle, FlasherConfig::MIN_VOLTAGE_IDLE_MV);
 
 transitionPhase(FlasherPhase::WIFI_START, "wifi start");
 ESP_LOGI(TAG, "Starting Wi-Fi SoftAP...");
 
 WiFi.disconnect(true);
 WiFi.mode(WIFI_OFF);
 delay(100);
 
 WiFi.mode(WIFI_AP);
 WiFi.setTxPower(WIFI_POWER_19_5dBm);
 
 WiFi.setSleep(false);
 ESP_LOGI(TAG, "Wi-Fi power save DISABLED for stable OTA");
 
 bool apStarted = WiFi.softAP(
 FlasherConfig::AP_SSID,
 FlasherConfig::AP_PASS,
 6,
 0,
 FlasherConfig::MAX_CONNECTIONS
 );
 
 if (!apStarted) {
 errorAndReboot(FlasherConfig::Error::WIFI_FAILED, "Failed to start Wi-Fi AP");
 }
 
 delay(1000);
 
 IPAddress local_IP(192, 168, 4, 1);
 IPAddress gateway(192, 168, 4, 1);
 IPAddress subnet(255, 255, 255, 0);
 WiFi.softAPConfig(local_IP, gateway, subnet);
 
 IPAddress ip = WiFi.softAPIP;
 ESP_LOGI(TAG, "Wi-Fi AP Started: SSID=%s, IP=%s", FlasherConfig::AP_SSID, ip.toString.c_str);
 
 ESP_LOGI(TAG, "Stage 2: Battery check under Wi-Fi load...");
 delay(500);
 
 g_state.vLoad = readBatteryVoltage;
 int32_t sag = (int32_t)g_state.vIdle - (int32_t)g_state.vLoad;
 
 ESP_LOGI(TAG, "Power: Idle=%lumV, Load=%lumV, Sag=%ldmV",
 g_state.vIdle, g_state.vLoad, sag);
 
 bool voltageFail = (g_state.vLoad < FlasherConfig::MIN_VOLTAGE_LOAD_MV);
 bool sagFail = (sag > (int32_t)FlasherConfig::MAX_VOLTAGE_SAG_MV);
 
 if (voltageFail || sagFail) {
 WiFi.softAPdisconnect(true);
 WiFi.mode(WIFI_OFF);
 
 char msg[80];
 snprintf(msg, sizeof(msg), "Power FAIL: Load=%lumV, Sag=%ldmV", g_state.vLoad, sag);
 errorAndReboot(FlasherConfig::Error::LOW_BATTERY_LOAD, msg);
 }
 
 ESP_LOGI(TAG, "Stage 2 PASS");
 
 setupServer;
 
 resetFlasherState;
 
 ESP_LOGI(TAG, "FLASHER MODE READY (- Explicit Erase)");
 ESP_LOGI(TAG, " Connect to Wi-Fi: %s", FlasherConfig::AP_SSID);
 ESP_LOGI(TAG, " Password: %s", FlasherConfig::AP_PASS);
 ESP_LOGI(TAG, " 1. POST /start with X-Session-ID, X-Total-Size");
 ESP_LOGI(TAG, " 2. Poll /status until phase=RECEIVING");
 ESP_LOGI(TAG, " 3. POST chunks to /update_chunk");
 
 
 while (true) {
 if (g_server != nullptr) {
 g_server->handleClient;
 }
 
 if (g_pendingAction != FlasherAction::NONE) {
 esp_task_wdt_reset;
 
 FlasherAction action = g_pendingAction;
 g_pendingAction = FlasherAction::NONE;
 
 switch (action) {
 case FlasherAction::ABORT_RESET:
 ESP_LOGW(TAG, "Executing deferred ABORT_RESET...");
 resetFlasherState;
 if (led) led->setColor(0xFFA500);
 ESP_LOGI(TAG, "State reset complete. Ready for new upload.");
 break;
 
 case FlasherAction::START_SESSION: {
 ESP_LOGI(TAG, "Executing deferred START_SESSION...");
 ESP_LOGI(TAG, " Session ID: %lu", g_pendingSessionId);
 ESP_LOGI(TAG, " Total Size: %lu bytes", g_pendingTotalSize);
 
 transitionPhase(FlasherPhase::ERASING, "start session");
 strncpy(g_state.message, "Erasing flash...", sizeof(g_state.message));
 
 if (led) led->setColor(0x800080);
 
 g_state.sessionId = g_pendingSessionId;
 g_state.total = g_pendingTotalSize;
 g_state.written = 0;
 g_state.lastChunkIndex = -1;
 g_state.lastActivityMs = millis;
 g_state.lastProgressMs = 0;
 resetPendingChunk;
 
 g_pendingSessionId = 0;
 g_pendingTotalSize = 0;
 
 ESP_LOGI(TAG, ">>> FLASH ERASE STARTING <<<");
 ESP_LOGI(TAG, "Target size: %lu bytes. This may take 3-10 seconds...", g_state.total);
 
 esp_task_wdt_reset;
 
 unsigned long eraseStart = millis;
 
 bool beginSuccess = Update.begin(g_state.total, U_FLASH);
 
 esp_task_wdt_reset;
 
 unsigned long eraseDuration = millis - eraseStart;
 
 if (beginSuccess) {
 ESP_LOGI(TAG, ">>> FLASH ERASE COMPLETE <<<");
 ESP_LOGI(TAG, " Duration: %lu ms", eraseDuration);
 
 vTaskDelay(pdMS_TO_TICKS(10));
 
 transitionPhase(FlasherPhase::RECEIVING, "erase complete");
 g_state.uploadStarted = true;
 strncpy(g_state.message, "Ready for chunks", sizeof(g_state.message));
 g_state.lastProgressMs = millis;
 
 if (led) led->setColor(0x0000FF);
 
 ESP_LOGI(TAG, "Phase: RECEIVING - Ready for chunks");
 
 } else {
 ESP_LOGE(TAG, ">>> FLASH ERASE FAILED <<<");
 ESP_LOGE(TAG, " Update.begin error: %d", Update.getError);
 
 setErrorState(FlasherConfig::Error::FLASH_BEGIN_FAILED,
 "Erase failed", 500);
 snprintf(g_state.message, sizeof(g_state.message),
 "Erase failed: %d", Update.getError);
 
 if (led) led->setColor(0xFF0000);
 
 g_state.sessionId = 0;
 g_state.total = 0;
 }
 break;
 }
 
 case FlasherAction::REBOOT_SUCCESS:
 ESP_LOGI(TAG, "Executing deferred REBOOT...");
 delay(2000);
 ESP_LOGI(TAG, "Update complete. Rebooting now...");
 Serial.flush;
 ESP.restart;
 break;
 
 case FlasherAction::NONE:
 default:
 break;
 }
 }

 processPendingChunk;
 
 esp_task_wdt_reset;
 
 uint32_t now = millis;
 
 if ((g_state.phase == FlasherPhase::RECEIVING || g_state.phase == FlasherPhase::ERASING) &&
 (now - g_state.lastActivityMs > 30000)) {

 ESP_LOGW(TAG, "WATCHDOG: Session timed out (30s without activity)");
 setErrorState(FlasherConfig::Error::TIMEOUT_IDLE, "Session timed out", 408);
 Update.abort;
 resetPendingChunk;
 }
 
 if (g_state.phase == FlasherPhase::READY &&
 (now - g_state.lastActivityMs > FlasherConfig::IDLE_TIMEOUT_MS)) {
 ESP_LOGW(TAG, "WATCHDOG: Idle Timeout (%lu ms). Rebooting to save battery.",
 FlasherConfig::IDLE_TIMEOUT_MS);
 ESP.restart;
 }

 if (g_state.phase == FlasherPhase::RECEIVING &&
 g_state.uploadStarted &&
 g_state.lastProgressMs > 0 &&
 (now - g_state.lastProgressMs > FlasherConfig::PROGRESS_TIMEOUT_MS)) {

 ESP_LOGW(TAG, "WATCHDOG: Upload stalled (%lu ms without progress)",
 FlasherConfig::PROGRESS_TIMEOUT_MS);
 setErrorState(FlasherConfig::Error::TIMEOUT_PROGRESS, "Upload stalled", 408);
 Update.abort;
 resetPendingChunk;
 }

 if (g_state.phase == FlasherPhase::RECEIVING &&
 g_pendingChunk.state == ChunkBufferState::RECEIVING &&
 g_pendingChunk.startMs > 0 &&
 (now - g_pendingChunk.startMs > FlasherConfig::CHUNK_RECEIVE_TIMEOUT_MS)) {

 ESP_LOGW(TAG, "WATCHDOG: Chunk receive timeout");
 setErrorState(FlasherConfig::Error::TIMEOUT_PROGRESS, "Chunk receive timeout", 408);
 Update.abort;
 resetPendingChunk;
 }

 if (g_state.phase == FlasherPhase::RECEIVING &&
 g_pendingChunk.state == ChunkBufferState::QUEUED &&
 g_pendingChunk.startMs > 0 &&
 (now - g_pendingChunk.startMs > FlasherConfig::CHUNK_WRITE_TIMEOUT_MS)) {

 ESP_LOGW(TAG, "WATCHDOG: Chunk write timeout");
 setErrorState(FlasherConfig::Error::TIMEOUT_PROGRESS, "Chunk write timeout", 408);
 Update.abort;
 resetPendingChunk;
 }
 
 static unsigned long lastBattCheck = 0;
 if (now - lastBattCheck > 10000) {
 lastBattCheck = now;
 
 uint32_t currentBatt = readBatteryVoltageInternal;
 
 if (g_state.uploadStarted && 
 g_state.phase == FlasherPhase::RECEIVING &&
 currentBatt < FlasherConfig::MIN_VOLTAGE_LOAD_MV) {
 
 ESP_LOGW(TAG, "WATCHDOG: Battery dropped during upload: %lumV",
 currentBatt);
 Update.abort;
 
 setErrorState(FlasherConfig::Error::LOW_BATTERY_LOAD, "Battery dropped", 500);
 snprintf(g_state.message, sizeof(g_state.message),
 "Battery dropped: %lumV", currentBatt);
 
 if (led) led->setColor(0xFF0000);
 }
 }
 
 vTaskDelay(pdMS_TO_TICKS(5));
 }
}
