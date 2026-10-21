/**
 * ESP32: web UI on a dedicated FreeRTOS task (prio 0, core 1) so loopWebserver()
 * cannot starve checkRX(). nRF52 keeps sync web in nrf52loop (stubs below).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Create mesh_web once (safe to call repeatedly). ESP32 only; nRF no-op. */
void webTaskStart(void);

/** Ask mesh_web to stop the HTTP server (single owner). Loop must not call stopWebserver(). */
void webTaskRequestStop(void);

/** Drain web→loop command queue; call once per esp32loop pass. */
void webCmdDrain(void);

/**
 * From web task: enqueue --command for loop. From loop/other: run commandAction now.
 * Returns false if queue full (caller should HTTP 503).
 */
bool webSubmitCommand(char *cmd);

/**
 * From web task: enqueue mesh send for loop. From loop/other: sendMessage now.
 * Returns false if queue full.
 */
bool webSubmitSend(char *msg, int len);

/** From web task: enqueue save_settings for loop. Returns false if queue full. */
bool webSubmitSaveSettings(void);

/** Shared mheard / neighbours / rxlog mutex. ms=0 = try once. */
bool webDataLockMs(uint32_t ms);
void webDataUnlock(void);

/** True when the calling task is mesh_web (ESP32). */
bool webTaskIsCurrent(void);

#ifdef __cplusplus
}
#endif
