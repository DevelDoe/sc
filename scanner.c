#include <json-c/json.h>
#include <libwebsockets.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// Constants
#define DEBUG_MODE 0
#define MAX_TRADES 300
#define MAX_SYMBOLS 50      // Maximum number of symbols to track
#define MOVE 0.01           // 1% price movement
#define DEBOUNCE_TIME 3000  // 3 seconds in milliseconds

// Globals
// Global WebSocket connection for local server
struct lws *wsi_local = NULL;
struct lws *wsi_finnhub = NULL;
char **symbols = NULL;
int num_symbols = 0;

#define LOG_DEBUG(fmt, ...)                         \
    do {                                            \
        if (DEBUG_MODE) printf(fmt, ##__VA_ARGS__); \
    } while (0)

// Free the symbols array
void free_symbols() {
    if (symbols) {
        for (int i = 0; i < num_symbols; i++) {
            free(symbols[i]);
        }
        free(symbols);
        symbols = NULL;
        num_symbols = 0;
    }
}

// Trade data structure
// Trade data structure
typedef struct {
    double price;
    int volume;  // ✅ Add this line to store volume
    unsigned long timestamp;
} Trade;

Trade trade_data[MAX_SYMBOLS][MAX_TRADES] = {0};
int trade_index[MAX_SYMBOLS] = {0};
unsigned long last_alert_time[MAX_SYMBOLS] = {0};
double last_checked_price[MAX_SYMBOLS] = {0};

// Per-session data for Finnhub
typedef struct {
    int sub_index;
} per_session_data;

// Function to store trade data
void store_trade(int symbol_index, double price, int volume) {
    if (symbol_index < 0 || symbol_index >= MAX_SYMBOLS) {
        printf("[ERROR] store_trade(): Invalid symbol_index = %d\n", symbol_index);
        return;
    }

    unsigned long timestamp = (unsigned long)(time(NULL));
    int idx = trade_index[symbol_index];

    if (idx < 0 || idx >= MAX_TRADES) {
        printf("[ERROR] store_trade(): Invalid trade_index[%d] = %d\n", symbol_index, idx);
        return;
    }

    LOG_DEBUG("[DEBUG] Storing trade | Symbol: %s | Price: %.4f | Volume: %d | Timestamp: %lu | Index: %d\n", symbols[symbol_index], price, volume, timestamp, idx);

    // Store new trade
    trade_data[symbol_index][idx].price = price;
    trade_data[symbol_index][idx].volume = volume;  // Store volume
    trade_data[symbol_index][idx].timestamp = timestamp;

    // Move index only if last trade was from a different second
    int prev_idx = (idx - 1 + MAX_TRADES) % MAX_TRADES;
    if (trade_data[symbol_index][prev_idx].timestamp != timestamp) {
        trade_index[symbol_index] = (idx + 1) % MAX_TRADES;
    }
}

void send_alert_to_server(int is_increase, double percentage, double price, int volume, const char *symbol) {
    if (!wsi_local) {
        printf("[ERROR] WebSocket to local server is not connected\n");
        return;
    }

    if (!wsi_local || lws_partial_buffered(wsi_local)) {
        printf("[ERROR] WebSocket is not writable. Skipping alert send.\n");
        return;
    }

    // Create JSON alert message
    struct json_object *alert_json = json_object_new_object();
    json_object_object_add(alert_json, "client_id", json_object_new_string("scanner"));

    // Ensure symbol is valid
    if (!symbol) {
        printf("[ERROR] send_alert_to_server(): NULL symbol\n");
        json_object_put(alert_json);
        return;
    }

    // Create the "data" object
    struct json_object *data_json = json_object_new_object();
    json_object_object_add(data_json, "symbol", json_object_new_string(symbol));
    json_object_object_add(data_json, "direction", json_object_new_string(is_increase ? "UP" : "DOWN"));
    json_object_object_add(data_json, "change_percent", json_object_new_double(percentage));
    json_object_object_add(data_json, "current_price", json_object_new_double(price));
    json_object_object_add(data_json, "trade_volume", json_object_new_int(volume));

    // Add "data" object to main message
    json_object_object_add(alert_json, "data", data_json);

    // Convert JSON to string
    const char *json_str = json_object_to_json_string(alert_json);

    // Allocate buffer with LWS_PRE padding
    unsigned char buf[LWS_PRE + 512];
    unsigned char *p = &buf[LWS_PRE];
    size_t msg_len = strlen(json_str);
    memcpy(p, json_str, msg_len);

    if (lws_write(wsi_local, p, msg_len, LWS_WRITE_TEXT) < 0) {
        printf("[ERROR] Failed to send WebSocket message.\n");
    } else {
        printf("[SCANNER] Sent alert: %s\n", json_str);
    }

    // Cleanup JSON object
    json_object_put(alert_json);
}

DWORD WINAPI monitor_thread(LPVOID lpParam) {
    while (1) {
        Sleep(1000);  // Check every second

        for (int symbol_index = 0; symbol_index < num_symbols; symbol_index++) {
            if (symbol_index < 0 || symbol_index >= MAX_SYMBOLS) {
                printf("[ERROR] monitor_thread(): Invalid symbol_index = %d\n", symbol_index);
                continue;
            }

            int last_trade_idx = (trade_index[symbol_index] - 1 + MAX_TRADES) % MAX_TRADES;
            if (last_trade_idx < 0 || last_trade_idx >= MAX_TRADES) {
                printf("[ERROR] monitor_thread(): Invalid last_trade_idx = %d for symbol %d\n", last_trade_idx, symbol_index);
                continue;
            }

            double current_price = trade_data[symbol_index][last_trade_idx].price;
            int current_volume = trade_data[symbol_index][last_trade_idx].volume;  // Get volume

            if (current_price <= 0) continue;

            double old_price = last_checked_price[symbol_index];

            if (old_price == 0) {
                last_checked_price[symbol_index] = current_price;
                continue;
            }

            double change = ((current_price - old_price) / old_price) * 100.0;
            unsigned long current_time = (unsigned long)(time(NULL) * 1000);

            if (fabs(change) >= MOVE && current_time - last_alert_time[symbol_index] >= DEBOUNCE_TIME) {
                int is_increase = change > 0;
                printf("%s: %s %.2f%% | Price: %.2f | Volume: %d\n", symbols[symbol_index], is_increase ? "UP" : "DOWN", fabs(change), current_price, current_volume);

                // Pass the symbol as the fifth argument
                send_alert_to_server(is_increase, fabs(change), current_price, current_volume, symbols[symbol_index]);
                last_alert_time[symbol_index] = current_time;
            }

            last_checked_price[symbol_index] = current_price;
        }
    }
    return 0;
}

// Local server callback
// Local server callback
static int callback_local_server(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("[SCANNER] WebSocket connected to local server\n");
            wsi_local = wsi;  // Store WebSocket connection

            // Send initial registration
            {
                char register_msg[64];
                snprintf(register_msg, sizeof(register_msg), "{\"client_id\":\"scanner\"}");

                unsigned char buf[LWS_PRE + 64];
                unsigned char *p = &buf[LWS_PRE];
                size_t msg_len = strlen(register_msg);
                memcpy(p, register_msg, msg_len);

                if (lws_write(wsi, p, msg_len, LWS_WRITE_TEXT) < 0) {
                    printf("[ERROR] Failed to send WebSocket registration\n");
                } else {
                    printf("[SCANNER] Registered client_id with WebSocket server\n");
                }
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if (!in || len == 0) {
                printf("[ERROR] Received empty WebSocket message\n");
                break;
            }

            struct json_object *msg = json_tokener_parse((char *)in);
            if (!msg) {
                printf("[ERROR] Failed to parse WebSocket message: %.*s\n", (int)len, (char *)in);
                break;
            }

            struct json_object *symbols_json;
            if (json_object_object_get_ex(msg, "symbols", &symbols_json)) {
                // Free old symbols before updating
                free_symbols();

                int received_symbols = json_object_array_length(symbols_json);
                num_symbols = received_symbols > MAX_SYMBOLS ? MAX_SYMBOLS : received_symbols;  // Enforce MAX_SYMBOLS limit

                symbols = malloc(num_symbols * sizeof(char *));
                if (!symbols) {
                    printf("[ERROR] Failed to allocate memory for symbols\n");
                    json_object_put(msg);
                    return -1;
                }

                printf("[LOCAL SERVER] Symbols updated (%d):\n", num_symbols);

                for (int i = 0; i < num_symbols; i++) {
                    symbols[i] = strdup(json_object_get_string(json_object_array_get_idx(symbols_json, i)));
                    if (!symbols[i]) {
                        printf("[ERROR] Failed to allocate memory for symbol %d\n", i);
                    } else {
                        printf(" - %s\n", symbols[i]);
                    }
                }

                // Trigger re-subscription on Finnhub
                if (wsi_finnhub) {
                    lws_callback_on_writable(wsi_finnhub);
                }
            } else {
                printf("[LOCAL SERVER] No symbols found in message\n");
            }

            json_object_put(msg);
            break;
        }

        case LWS_CALLBACK_CLOSED:
            if (wsi == wsi_local) {
                wsi_local = NULL;  // Reset when WebSocket closes
                printf("[SCANNER] WebSocket disconnected from local server\n");
            }
            break;

        default:
            break;
    }
    return 0;
}

static int callback_finnhub(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    per_session_data *pss = (per_session_data *)user;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("[FINNHUB] Connected\n");
            pss->sub_index = 0;
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (pss->sub_index < num_symbols) {
                char subscribe_msg[128];
                snprintf(subscribe_msg, sizeof(subscribe_msg), "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", symbols[pss->sub_index]);

                unsigned char buf[LWS_PRE + 256];
                unsigned char *p = &buf[LWS_PRE];
                size_t msg_len = strlen(subscribe_msg);
                memcpy(p, subscribe_msg, msg_len);

                lws_write(wsi, p, msg_len, LWS_WRITE_TEXT);
                printf("[FINNHUB] Subscribed to: %s\n", symbols[pss->sub_index]);

                pss->sub_index++;

                // Request the next writable callback if there are more symbols to subscribe
                if (pss->sub_index < num_symbols) {
                    lws_callback_on_writable(wsi);
                }
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            LOG_DEBUG("[FINNHUB] Trade data received: %.*s\n", (int)len, (char *)in);

            // Check if the message is a ping message
            if (len == 4 && strncmp((char *)in, "ping", 4) == 0) {
                LOG_DEBUG("[FINNHUB] Ignored ping message.\n");
                break;  // Ignore ping messages
            }

            // Parse incoming JSON message
            struct json_object *msg = json_tokener_parse((char *)in);
            if (!msg) {
                printf("[ERROR] Failed to parse JSON message: %.*s\n", (int)len, (char *)in);
                break;
            }

            // Check if the JSON contains a "type" field
            struct json_object *type_obj;
            if (json_object_object_get_ex(msg, "type", &type_obj)) {
                const char *msg_type = json_object_get_string(type_obj);
                if (strcmp(msg_type, "ping") == 0) {
                    LOG_DEBUG("[FINNHUB] Ignored ping message.\n");
                    json_object_put(msg);
                    break;
                } else if (strcmp(msg_type, "error") == 0) {
                    printf("[FINNHUB] Error message received: %s\n", json_object_to_json_string(msg));
                    json_object_put(msg);
                    break;
                }
            }

            // Extract "data" array from JSON
            struct json_object *data_array;
            if (!json_object_object_get_ex(msg, "data", &data_array) || json_object_get_type(data_array) != json_type_array) {
                printf("[ERROR] JSON does not contain a valid 'data' array. Ignoring message.\n");
                json_object_put(msg);
                return 0;  // Return early
            }

            for (size_t i = 0; i < json_object_array_length(data_array); i++) {
                struct json_object *trade = json_object_array_get_idx(data_array, i);
                struct json_object *symbol_obj, *price_obj, *volume_obj;

                // Ensure all required fields are present
                if (json_object_object_get_ex(trade, "s", &symbol_obj) && json_object_object_get_ex(trade, "p", &price_obj) && json_object_object_get_ex(trade, "v", &volume_obj)) {
                    const char *symbol = json_object_get_string(symbol_obj);
                    double price = json_object_get_double(price_obj);
                    int volume = json_object_get_int(volume_obj);

                    LOG_DEBUG("[DEBUG] Received Trade: Symbol=%s | Price=%.4f | Volume=%d\n", symbol, price, volume);

                    // Find the symbol index
                    for (int j = 0; j < num_symbols; j++) {
                        if (strcmp(symbols[j], symbol) == 0) {
                            store_trade(j, price, volume);
                            break;
                        }
                    }
                } else {
                    printf("[ERROR] Missing required trade data fields. Skipping entry.\n");
                }
            }

            // Free JSON object
            json_object_put(msg);
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            printf("[FINNHUB] Connection error\n");
            break;

        case LWS_CALLBACK_CLOSED:
            printf("[FINNHUB] Disconnected\n");
            break;

        default:
            break;
    }
    return 0;
}

int main() {
    // Create a single context
    struct lws_context_creation_info info = {0};
    struct lws_protocols protocols[] = {{"local-server-protocol", callback_local_server, 0, 0}, {"finnhub-protocol", callback_finnhub, sizeof(per_session_data), 0}, {NULL, NULL, 0, 0}};
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "[ERROR] Failed to create context\n");
        return 1;
    }

    // Connect to the local WebSocket server
    struct lws_client_connect_info conn_local = {0};
    conn_local.context = context;
    conn_local.address = "192.168.1.17";
    conn_local.port = 8000;
    conn_local.path = "/ws";
    conn_local.host = conn_local.address;
    conn_local.origin = conn_local.address;
    conn_local.protocol = "local-server-protocol";

    wsi_local = lws_client_connect_via_info(&conn_local);
    if (!wsi_local) {
        fprintf(stderr, "[ERROR] Failed to connect to local WebSocket server\n");
        lws_context_destroy(context);
        return 1;
    }

    // Send the initial registration message (client_id)
    char register_msg[64];
    snprintf(register_msg, sizeof(register_msg), "{\"client_id\":\"scanner\"}");

    unsigned char buf[LWS_PRE + 64];
    unsigned char *p = &buf[LWS_PRE];
    size_t msg_len = strlen(register_msg);
    memcpy(p, register_msg, msg_len);

    // Send registration message after connection is established
    lws_write(wsi_local, p, msg_len, LWS_WRITE_TEXT);
    printf("[SCANNER] Sent client_id registration\n");

    // Connect to Finnhub
    struct lws_client_connect_info conn_finnhub = {0};
    conn_finnhub.context = context;
    conn_finnhub.address = "ws.finnhub.io";
    conn_finnhub.port = 443;
    conn_finnhub.path = "/?token=crhlrm9r01qjv9rl4bhgcrhlrm9r01qjv9rl4bi0";
    conn_finnhub.host = conn_finnhub.address;
    conn_finnhub.origin = conn_finnhub.address;
    conn_finnhub.protocol = "finnhub-protocol";
    conn_finnhub.ssl_connection = LCCSCF_USE_SSL;

    wsi_finnhub = lws_client_connect_via_info(&conn_finnhub);
    if (!wsi_finnhub) {
        fprintf(stderr, "[ERROR] Failed to connect to Finnhub\n");
        lws_context_destroy(context);
        return 1;
    }

    // Start the monitor thread
    HANDLE monitorHandle = CreateThread(NULL, 0, monitor_thread, NULL, 0, NULL);
    if (!monitorHandle) {
        fprintf(stderr, "[ERROR] Failed to create monitor thread\n");
        lws_context_destroy(context);
        return 1;
    }

    // Event loop
    while (1) {
        lws_service(context, 50);
    }

    // Cleanup (optional in this infinite loop)
    lws_context_destroy(context);
    if (!symbols) return 0;
    free_symbols();
    return 0;
}