/**
 * @file rpc_server.c
 * Device control JSON-RPC server (port 1234).
 *
 * Registers RPC methods for LED, DHT11, SG90, and AP3216C.
 * Uses jsonrpc-c + libev for event-driven TCP serving.
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "jsonrpc-c.h"
#include "rpc_protocol.h"
#include "dev_led.h"
#include "dev_dht11.h"
#include "dev_sg90.h"
#include "dev_ap3216c.h"

static struct jrpc_server my_server;
static volatile int running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    LOG_INFO("Device server shutting down...");
    jrpc_server_stop(&my_server);
    running = 0;
}

/* ── RPC: led_set ──────────────────────────────────────────────────── */
cJSON *rpc_led_set(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx;
    cJSON *result = cJSON_CreateObject();

    cJSON *a = cJSON_GetArrayItem(params, 0);
    cJSON *b = cJSON_GetArrayItem(params, 1);
    if (!a || !b) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "missing params: [led_index, on/off]");
        return result;
    }

    int index = a->valueint;
    int on    = b->valueint;
    int ret   = led_set(index, on);

    cJSON_AddBoolToObject(result, "ok", (ret == 0));
    return result;
}

/* ── RPC: led_get ──────────────────────────────────────────────────── */
cJSON *rpc_led_get(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx;
    cJSON *result = cJSON_CreateObject();

    cJSON *a = cJSON_GetArrayItem(params, 0);
    if (!a) {
        cJSON_AddBoolToObject(result, "ok", false);
        return result;
    }

    int index = a->valueint;
    int on = 0;
    int ret = led_get(index, &on);

    cJSON_AddBoolToObject(result, "ok", (ret == 0));
    cJSON_AddBoolToObject(result, "on", (on != 0));
    return result;
}

/* ── RPC: dht11_read ───────────────────────────────────────────────── */
cJSON *rpc_dht11_read(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx;
    (void)params;
    cJSON *result = cJSON_CreateObject();

    int humidity = 0, temp = 0;
    int ret = dht11_read(&humidity, &temp);

    if (ret == 0) {
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddNumberToObject(result, "humidity", humidity);
        cJSON_AddNumberToObject(result, "temp", temp);
    } else {
        cJSON_AddBoolToObject(result, "ok", false);
    }
    return result;
}

/* ── RPC: sg90_set ─────────────────────────────────────────────────── */
cJSON *rpc_sg90_set(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx;
    cJSON *result = cJSON_CreateObject();

    cJSON *a = cJSON_GetArrayItem(params, 0);
    if (!a) {
        cJSON_AddBoolToObject(result, "ok", false);
        return result;
    }

    int angle = a->valueint;
    int ret = sg90_set(angle);

    cJSON_AddBoolToObject(result, "ok", (ret == 0));
    cJSON_AddNumberToObject(result, "angle", angle);
    return result;
}

/* ── RPC: ap3216c_read ─────────────────────────────────────────────── */
cJSON *rpc_ap3216c_read(jrpc_context *ctx, cJSON *params, cJSON *id)
{
    (void)ctx;
    (void)params;
    cJSON *result = cJSON_CreateObject();

    ap3216c_data_t data;
    int ret = ap3216c_read(&data);

    if (ret == 0) {
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddNumberToObject(result, "als", data.als);
        cJSON_AddNumberToObject(result, "ps",  data.ps);
        cJSON_AddNumberToObject(result, "ir",  data.ir);
    } else {
        cJSON_AddBoolToObject(result, "ok", false);
    }
    return result;
}

int main(void)
{
    LOG_INFO("====================================");
    LOG_INFO("Smart Screen — Device RPC Server");
    LOG_INFO("Port: %d", DEVICE_SERVER_PORT);
    LOG_INFO("====================================");

    /* Initialize hardware */
    led_init();
    dht11_init();
    sg90_init();
    ap3216c_init();

    /* Set up signal handlers for graceful shutdown */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize and configure JSON-RPC server */
    jrpc_server_init(&my_server, DEVICE_SERVER_PORT);

    /* Register procedures */
    jrpc_register_procedure(&my_server, rpc_led_set,      RPC_METHOD_LED_SET,      NULL);
    jrpc_register_procedure(&my_server, rpc_led_get,      RPC_METHOD_LED_GET,      NULL);
    jrpc_register_procedure(&my_server, rpc_dht11_read,   RPC_METHOD_DHT11_READ,   NULL);
    jrpc_register_procedure(&my_server, rpc_sg90_set,     RPC_METHOD_SG90_SET,     NULL);
    jrpc_register_procedure(&my_server, rpc_ap3216c_read, RPC_METHOD_AP3216C_READ, NULL);

    LOG_INFO("Registered %d procedures", 5);
    LOG_INFO("Server ready. Waiting for connections...");

    /* Run event loop (blocking until stopped) */
    jrpc_server_run(&my_server);

    /* Cleanup */
    jrpc_server_destroy(&my_server);
    led_exit();
    dht11_exit();
    sg90_exit();
    ap3216c_exit();

    LOG_INFO("Device server stopped.");
    return 0;
}
