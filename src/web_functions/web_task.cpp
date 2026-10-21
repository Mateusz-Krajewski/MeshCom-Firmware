#include "web_task.h"

#include <Arduino.h>
#include <string.h>

#include <configuration.h>
#include <loop_functions.h>
#include <loop_functions_extern.h>
#include <command_functions.h>
#include "web_functions.h"
#include "meshcom_settings.h"

extern bool bweb_server_running;

#if defined(ESP32)
void save_settings(void);
#else
bool save_settings(void);
#endif

#if defined(ESP32)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

/* iWlanWait: loop_functions_extern.h */

enum WebCmdKind : uint8_t
{
    WEB_CMD_COMMAND = 1,
    WEB_CMD_SEND = 2,
    WEB_CMD_SAVE = 3
};

struct WebCmdItem
{
    WebCmdKind kind;
    char text[200];
    int len;   // for SEND
};

/* 10 KB was enough after warm reboot but first cold boot (MDNS/WiFi + ipc1)
 * tripped a stack canary on Heltec Wireless Tracker. Match audio task budget. */
static const uint32_t WEB_TASK_STACK = (16 * 1024);
static const UBaseType_t WEB_TASK_PRIO = 0;
static const BaseType_t WEB_TASK_CORE = 1;
static const UBaseType_t WEB_CMD_DEPTH = 8;

static TaskHandle_t s_webTask = NULL;
static QueueHandle_t s_webCmdQ = NULL;
static SemaphoreHandle_t s_webDataMux = NULL;
static volatile bool s_request_stop = false;
static bool s_web_was_up = false;
static volatile uint32_t s_webCmdPending = 0;

static bool webCmdEnqueue(const WebCmdItem &item)
{
    if(s_webCmdQ == NULL)
        return false;
    if(xQueueSend(s_webCmdQ, &item, 0) != pdTRUE)
    {
        Serial.printf("[WEB];cmd;drop;kind;%u\n", (unsigned)item.kind);
        return false;
    }
    s_webCmdPending++;
    return true;
}

/* After enqueue from mesh_web: yield so loop (prio 1) can drain before verify-after. */
static bool webCmdWaitDrained(uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while(s_webCmdPending > 0 && (uint32_t)(millis() - t0) < timeout_ms)
        vTaskDelay(pdMS_TO_TICKS(1));
    return s_webCmdPending == 0;
}

bool webTaskIsCurrent(void)
{
    return s_webTask != NULL && xTaskGetCurrentTaskHandle() == s_webTask;
}

bool webDataLockMs(uint32_t ms)
{
    if(s_webDataMux == NULL)
        return true;
    TickType_t t = (ms == 0) ? 0 : pdMS_TO_TICKS(ms);
    return xSemaphoreTake(s_webDataMux, t) == pdTRUE;
}

void webDataUnlock(void)
{
    if(s_webDataMux != NULL)
        xSemaphoreGive(s_webDataMux);
}

bool webSubmitCommand(char *cmd)
{
    if(cmd == NULL || cmd[0] == 0x00)
        return false;
    if(webTaskIsCurrent())
    {
        WebCmdItem item;
        memset(&item, 0, sizeof(item));
        item.kind = WEB_CMD_COMMAND;
        snprintf(item.text, sizeof(item.text), "%s", cmd);
        item.len = (int)strlen(item.text);
        if(!webCmdEnqueue(item))
            return false;
        return webCmdWaitDrained(800);
    }
    commandAction(cmd, isPhoneReady == 1);
    return true;
}

bool webSubmitSend(char *msg, int len)
{
    if(msg == NULL || len <= 0)
        return false;
    if(webTaskIsCurrent())
    {
        WebCmdItem item;
        memset(&item, 0, sizeof(item));
        item.kind = WEB_CMD_SEND;
        if(len >= (int)sizeof(item.text))
            len = (int)sizeof(item.text) - 1;
        memcpy(item.text, msg, (size_t)len);
        item.text[len] = 0x00;
        item.len = len;
        if(!webCmdEnqueue(item))
            return false;
        return webCmdWaitDrained(800);
    }
    hasMsgFromPhone = true;
    setMsgOrigin(ORIGIN_WEB);
    (void)sendMessage(msg, len);
    setMsgOrigin(ORIGIN_NONE);
    hasMsgFromPhone = false;
    return true;
}

bool webSubmitSaveSettings(void)
{
    if(webTaskIsCurrent())
    {
        WebCmdItem item;
        memset(&item, 0, sizeof(item));
        item.kind = WEB_CMD_SAVE;
        if(!webCmdEnqueue(item))
            return false;
        return webCmdWaitDrained(1500);
    }
    save_settings();
    return true;
}

void webCmdDrain(void)
{
    if(s_webCmdQ == NULL)
        return;
    WebCmdItem item;
    while(xQueueReceive(s_webCmdQ, &item, 0) == pdTRUE)
    {
        switch(item.kind)
        {
            case WEB_CMD_COMMAND:
                commandAction(item.text, isPhoneReady == 1);
                break;
            case WEB_CMD_SEND:
                hasMsgFromPhone = true;
                setMsgOrigin(ORIGIN_WEB);
                (void)sendMessage(item.text, item.len);
                setMsgOrigin(ORIGIN_NONE);
                hasMsgFromPhone = false;
                break;
            case WEB_CMD_SAVE:
                save_settings();
                break;
            default:
                break;
        }
        if(s_webCmdPending > 0)
            s_webCmdPending--;
    }
}

void webTaskRequestStop(void)
{
    s_request_stop = true;
}

static void mesh_web_task(void * /*arg*/)
{
    /* Yield before any WiFi/MDNS work so loop/WiFi (core 0) finish boot IPC. */
    vTaskDelay(pdMS_TO_TICKS(50));

    Serial.printf("[WEB];task;start;prio;%u;core;%d;stack;%u\n",
                  (unsigned)WEB_TASK_PRIO, (int)WEB_TASK_CORE,
                  (unsigned)WEB_TASK_STACK);

    uint32_t loops = 0;

    for(;;)
    {
        const bool want = bWEBSERVER && meshcom_settings.node_hasIPaddress && (iWlanWait == 0);
        const bool stop_req = s_request_stop;

        if(stop_req || !want)
        {
            if(s_web_was_up || bweb_server_running)
            {
                stopWebserver();
                s_web_was_up = false;
            }
            s_request_stop = false;
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }

        startWebserver();
        if(bweb_server_running)
        {
            s_web_was_up = true;
            loopWebserver();
        }

        if(loops == 0 || loops == 50)
        {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            Serial.printf("[WEB];task;hwm;words;%u;loops;%lu\n",
                          (unsigned)hwm, (unsigned long)loops);
        }
        loops++;

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void webTaskStart(void)
{
    if(s_webDataMux == NULL)
        s_webDataMux = xSemaphoreCreateMutex();
    if(s_webCmdQ == NULL)
        s_webCmdQ = xQueueCreate(WEB_CMD_DEPTH, sizeof(WebCmdItem));
    if(s_webTask != NULL)
        return;
    if(s_webCmdQ == NULL || s_webDataMux == NULL)
    {
        Serial.printf("[WEB];task;err;init\n");
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(
        mesh_web_task, "mesh_web", WEB_TASK_STACK, NULL,
        WEB_TASK_PRIO, &s_webTask, WEB_TASK_CORE);
    if(ok != pdPASS)
    {
        s_webTask = NULL;
        Serial.printf("[WEB];task;err;create\n");
    }
}

#else /* !ESP32 — nRF52: sync path stubs */

bool webTaskIsCurrent(void) { return false; }
bool webDataLockMs(uint32_t) { return true; }
void webDataUnlock(void) {}
void webTaskStart(void) {}
void webTaskRequestStop(void) { stopWebserver(); }
void webCmdDrain(void) {}

bool webSubmitCommand(char *cmd)
{
    if(cmd == NULL)
        return false;
    commandAction(cmd, isPhoneReady == 1);
    return true;
}

bool webSubmitSend(char *msg, int len)
{
    if(msg == NULL || len <= 0)
        return false;
    hasMsgFromPhone = true;
    setMsgOrigin(ORIGIN_WEB);
    (void)sendMessage(msg, len);
    setMsgOrigin(ORIGIN_NONE);
    hasMsgFromPhone = false;
    return true;
}

bool webSubmitSaveSettings(void)
{
    save_settings();
    return true;
}

#endif
