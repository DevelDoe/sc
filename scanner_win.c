#include <json-c/json.h>
#include <libwebsockets.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>  // For gettimeofday
#include <time.h>
#include <windows.h>
#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#define THREAD_FUNC DWORD WINAPI
#else
#include <pthread.h>
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#define THREAD_FUNC void *
#endif

// Function to get the current time in milliseconds
unsigned long get_current_time_ms() {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // Convert to milliseconds since Unix epoch
    return (uli.QuadPart / 10000) - 11644473600000ULL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

/* ----------------------------- Configuration ------------------------------ */
#define CLEANER_MODE 0  // Enable only cleaner logs
#define ALERTS_MODE 0   // Set to 1 to enable alert logs, 0 to disable
#define WS_LOG_MODE 1   // Set to 1 to enable WebSocket logs, 0 to disable
#define PROCESSING_MODE 1
#define DEBUG_MODE 0  // Disable normal debug logs

#define MAX_SYMBOLS 50
#define PRICE_MOVEMENT 1.0  // 1% price movement
#define DEBOUNCE_TIME 5000  // 3 seconds in milliseconds
#define MAX_TRADES 1000     // Upper limit for active trades to avoid memory overload
#define LOCAL_SERVER_URI "ws://192.168.1.17:8000/ws"
#define FINNHUB_URI "wss://ws.finnhub.io/?token=your_token"
#define MAX_QUEUE_SIZE 1024  // For both trade and alert queues

#define MIN_TRADE_VOLUME 1          // Ignore individual trades below this volume
#define MIN_CUMULATIVE_VOLUME 5000  // Only trigger alerts if cumulative volume is above this threshold

#ifndef LWS_USEC_PER_MSEC
#define LWS_USEC_PER_MSEC 1000
#endif

/* ----------------------------- LOGGING ------------------------------ */
#define LOG(fmt, ...) \
    if (CLEANER_MODE == 0) printf("[%s] " fmt, __func__, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    if (DEBUG_MODE) LOG(fmt, ##__VA_ARGS__)

#define LOG_CLEANER(fmt, ...) \
    if (CLEANER_MODE) printf("[%s] " fmt, __func__, ##__VA_ARGS__)

#define LOG_ALERTS(fmt, ...) \
    if (ALERTS_MODE) printf("[%s] " fmt, __func__, ##__VA_ARGS__)

#define LOG_WS(fmt, ...) \
    if (WS_LOG_MODE) printf("[%s] " fmt, __func__, ##__VA_ARGS__)

#define LOG_PROCESSING(fmt, ...) \
    if (PROCESSING_MODE) printf("[%s] " fmt, __func__, ##__VA_ARGS__)

/* ----------------------------- Data Structures ---------------------------- */

// Trade message (from Finnhub)
typedef struct {
    char symbol[32];
    double price;
    int volume;
    unsigned long timestamp;
} TradeMsg;

// Alert message to be sent to local server
typedef struct {
    int symbol_index;
    double change;  // Signed change (positive or negative)
    double price;
    int volume;
} AlertMsg;

// Thread-safe queue for trades
typedef struct {
    TradeMsg trades[MAX_QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} TradeQueue;

// Thread-safe queue for alerts
typedef struct {
    AlertMsg alerts[MAX_QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} AlertQueue;
#include <stdint.h>

typedef struct {
    char *symbols[MAX_SYMBOLS];
    int num_symbols;
    unsigned long last_alert_time[MAX_SYMBOLS];
    double last_alert_price[MAX_SYMBOLS];

    struct {
        double price;
        uint64_t timestamp;
        int volume;
    } trade_history[MAX_SYMBOLS][MAX_TRADES];

    int trade_count[MAX_SYMBOLS];
    int trade_head[MAX_SYMBOLS];
    unsigned long total_volume[MAX_SYMBOLS];

    struct lws *wsi_local;
    struct lws *wsi_finnhub;
    struct lws_context *context;

    TradeQueue trade_queue;
    AlertQueue alert_queue;

    pthread_mutex_t symbols_mutex;
    volatile int shutdown_flag;
    char scanner_id[64];

    // ✅ NEW: Track last received ping timestamp
    unsigned long last_ping_time;

    FinnhubSession finnhub_session;  // ✅ Add this line

} ScannerState;

// Session data for Finnhub connection
typedef struct {
    int sub_index;
} FinnhubSession;

/* ----------------------------- Queue Functions ---------------------------- */
// TradeQueue functions
static int trade_queue_empty(TradeQueue *q) { return q->head == q->tail; }

static int trade_queue_full(TradeQueue *q) { return ((q->tail + 1) % MAX_QUEUE_SIZE) == q->head; }

static void queue_push_trade(TradeQueue *q, TradeMsg *trade) {
    if (trade_queue_full(q)) {
        LOG("Trade queue full, dropping trade\n");
        return;
    }
    q->trades[q->tail] = *trade;
    q->tail = (q->tail + 1) % MAX_QUEUE_SIZE;
}

static void queue_pop_trade(TradeQueue *q, TradeMsg *trade) {
    if (trade_queue_empty(q)) return;
    *trade = q->trades[q->head];
    q->head = (q->head + 1) % MAX_QUEUE_SIZE;
}

// AlertQueue functions
static int alert_queue_empty(AlertQueue *q) { return q->head == q->tail; }

static int alert_queue_full(AlertQueue *q) { return ((q->tail + 1) % MAX_QUEUE_SIZE) == q->head; }

static void queue_push_alert(AlertQueue *q, AlertMsg *alert) {
    if (alert_queue_full(q)) {
        LOG("Alert queue full, dropping alert\n");
        return;
    }
    q->alerts[q->tail] = *alert;
    q->tail = (q->tail + 1) % MAX_QUEUE_SIZE;
}

static void queue_pop_alert(AlertQueue *q, AlertMsg *alert) {
    if (alert_queue_empty(q)) return;
    *alert = q->alerts[q->head];
    q->head = (q->head + 1) % MAX_QUEUE_SIZE;
}

/* ----------------------------- Initialization ----------------------------- */
static void initialize_state(ScannerState *state) {
    memset(state, 0, sizeof(*state));
    pthread_mutex_init(&state->symbols_mutex, NULL);

    // Initialize trade queue mutex/cond
    pthread_mutex_init(&state->trade_queue.mutex, NULL);
    pthread_cond_init(&state->trade_queue.cond, NULL);

    // Initialize alert queue mutex/cond
    pthread_mutex_init(&state->alert_queue.mutex, NULL);
    pthread_cond_init(&state->alert_queue.cond, NULL);

    state->last_ping_time = get_current_time_ms();
}

static void cleanup_state(ScannerState *state) {
    // Free symbols
    pthread_mutex_lock(&state->symbols_mutex);
    for (int i = 0; i < state->num_symbols; i++) {
        free(state->symbols[i]);
    }
    pthread_mutex_unlock(&state->symbols_mutex);
    pthread_mutex_destroy(&state->symbols_mutex);

    pthread_mutex_destroy(&state->trade_queue.mutex);
    pthread_cond_destroy(&state->trade_queue.cond);
    pthread_mutex_destroy(&state->alert_queue.mutex);
    pthread_cond_destroy(&state->alert_queue.cond);
}

/* ----------------------------- WebSocket Handlers ----------------------------- */
static int handle_local_server_connection(ScannerState *state) {
    if (!state || !state->context) {
        LOG_WS("Invalid state or context\n");
        return -1;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = state->context;
    ccinfo.address = "172.232.155.62";
    ccinfo.port = 8000;
    ccinfo.path = "/ws";
    ccinfo.host = ccinfo.address;
    ccinfo.origin = ccinfo.address;
    ccinfo.protocol = "local-server";
    ccinfo.ssl_connection = 0;

    state->wsi_local = lws_client_connect_via_info(&ccinfo);
    if (!state->wsi_local) {
        LOG_WS("Failed to connect to local server\n");
        return -1;
    }

    LOG_WS("Local server connection initiated\n");
    return 0;
}

static int handle_finnhub_connection(ScannerState *state) {
    if (!state || !state->context) {
        LOG_WS("Invalid state or context\n");
        return -1;
    }

    // Reset the subscription index for a fresh connection
    memset(&state->finnhub_session, 0, sizeof(FinnhubSession));

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = state->context;
    ccinfo.address = "ws.finnhub.io";
    ccinfo.port = 443;
    ccinfo.path = "/?token=crhlrm9r01qjv9rl4bhgcrhlrm9r01qjv9rl4bi0";
    ccinfo.host = ccinfo.address;
    ccinfo.origin = ccinfo.address;
    ccinfo.protocol = "finnhub";
    ccinfo.ssl_connection = LCCSCF_USE_SSL;

    // Important: Assign the session pointer here so it’s passed to the callback
    ccinfo.userdata = &state->finnhub_session;

    state->wsi_finnhub = lws_client_connect_via_info(&ccinfo);
    if (!state->wsi_finnhub) {
        LOG_WS("Failed to connect to Finnhub\n");
        return -1;
    }

    LOG_WS("Finnhub connection initiated\n");
    return 0;
}

/* ----------------------------- Alert Sending ----------------------------- */
static void send_alert(ScannerState *state, int symbol_idx, double change, double price, int volume) {
    if (symbol_idx < 0 || symbol_idx >= state->num_symbols || !state->symbols[symbol_idx]) {
        LOG_ALERTS("Invalid symbol index or null symbol\n");
        return;
    }

    const char *direction = (change > 0) ? "UP" : "DOWN";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"client_id\":\"%s\",\"data\":{"
             "\"symbol\":\"%s\",\"direction\":\"%s\","
             "\"change_percent\":%.2f,\"price\":%.2f,\"volume\":%d}}",
             state->scanner_id,  // Changed from "scanner" to state->scanner_id
             state->symbols[symbol_idx], direction, fabs(change), price, volume);

    unsigned char buf[LWS_PRE + 256];
    unsigned char *p = &buf[LWS_PRE];
    size_t len = strlen(payload);
    memcpy(p, payload, len);

    if (state->wsi_local && !lws_send_pipe_choked(state->wsi_local)) {
        lws_write(state->wsi_local, p, len, LWS_WRITE_TEXT);
        LOG_ALERTS("Alert sent: %s\n", payload);
    } else {
        LOG_ALERTS("Local server connection unavailable or choked\n");
    }
}

/* ----------------------------- Enqueue Trade ----------------------------- */
// Called by the Finnhub callback to enqueue a trade for processing.
void enqueue_trade(ScannerState *state, const char *symbol, double price, int volume) {
    if (!state || !symbol) {
        LOG("Invalid arguments in enqueue_trade\n");
        return;
    }

    TradeMsg trade;
    strncpy(trade.symbol, symbol, sizeof(trade.symbol) - 1);
    trade.symbol[sizeof(trade.symbol) - 1] = '\0';
    trade.price = price;
    trade.volume = volume;
    trade.timestamp = (unsigned long)time(NULL);

    pthread_mutex_lock(&state->trade_queue.mutex);
    if (trade_queue_full(&state->trade_queue)) {
        LOG("Trade queue full, dropping trade\n");
    } else {
        queue_push_trade(&state->trade_queue, &trade);
        pthread_cond_signal(&state->trade_queue.cond);
    }
    pthread_mutex_unlock(&state->trade_queue.mutex);
}

/* ----------------------------- WebSocket Callbacks ----------------------------- */
static int local_server_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    ScannerState *state = (ScannerState *)lws_context_user(lws_get_context(wsi));

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            LOG_WS("Connected to local server\n");
            state->wsi_local = wsi;
            state->last_ping_time = get_current_time_ms();
            {
                char register_msg[128];
                snprintf(register_msg, sizeof(register_msg), "{\"client_id\":\"%s\"}", state->scanner_id);
                unsigned char buf[LWS_PRE + 128];
                unsigned char *p = &buf[LWS_PRE];
                size_t msg_len = strlen(register_msg);
                memcpy(p, register_msg, msg_len);
                if (lws_write(wsi, p, msg_len, LWS_WRITE_TEXT) < 0)
                    LOG_WS("Failed to send registration message\n");
                else
                    LOG_WS("Sent registration message: %s\n", register_msg);
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            // Create null-terminated copy of the message
            char *msg_str = malloc(len + 1);
            strncpy(msg_str, (char *)in, len);
            msg_str[len] = '\0';

            // Try to parse as JSON
            struct json_object *root = json_tokener_parse(msg_str);
            if (root) {
                struct json_object *type_obj;

                if (json_object_object_get_ex(root, "type", &type_obj)) {
                    const char *msg_type = json_object_get_string(type_obj);
                    if (strcmp(msg_type, "ping") == 0) {
                        state->last_ping_time = get_current_time_ms();  // ✅ Update ping time
                        // Create pong message with client_id
                        const char *pong_fmt = "{\"type\":\"pong\",\"client_id\":\"%s\"}";
                        char pong_buffer[128];
                        int pong_len = snprintf(pong_buffer, sizeof(pong_buffer), pong_fmt, state->scanner_id);

                        // Validate buffer size
                        if (pong_len >= sizeof(pong_buffer)) {
                            LOG_WS("Pong buffer too small!\n");
                            break;
                        }

                        // Create properly sized buffer
                        unsigned char buf[LWS_PRE + pong_len];
                        unsigned char *p = &buf[LWS_PRE];
                        memcpy(p, pong_buffer, pong_len);

                        lws_write(wsi, p, pong_len, LWS_WRITE_TEXT);
                        LOG_WS("Sent pong response: %s\n", pong_buffer);

                        json_object_put(root);
                        free(msg_str);
                        break;
                    }
                }

                json_object_put(root);
            }

            // If not a ping, check for symbols array
            struct json_object *msg = json_tokener_parse(msg_str);
            struct json_object *symbols_array;
            if (json_object_object_get_ex(msg, "symbols", &symbols_array)) {
                pthread_mutex_lock(&state->symbols_mutex);

                // Unsubscribe from old symbols
                if (state->wsi_finnhub) {
                    for (int i = 0; i < state->num_symbols; i++) {
                        char unsubscribe_msg[128];
                        snprintf(unsubscribe_msg, sizeof(unsubscribe_msg), "{\"type\":\"unsubscribe\",\"symbol\":\"%s\"}", state->symbols[i]);
                        unsigned char buf[LWS_PRE + 128];
                        unsigned char *p = &buf[LWS_PRE];
                        size_t msg_len = strlen(unsubscribe_msg);
                        memcpy(p, unsubscribe_msg, msg_len);
                        lws_write(state->wsi_finnhub, p, msg_len, LWS_WRITE_TEXT);
                        LOG_WS("Unsubscribed from: %s\n", unsubscribe_msg);
                    }
                }

                // Free old symbols
                for (int i = 0; i < state->num_symbols; i++) {
                    free(state->symbols[i]);
                    state->symbols[i] = NULL;
                }

                // Update symbols list
                state->num_symbols = json_object_array_length(symbols_array);
                if (state->num_symbols > MAX_SYMBOLS) state->num_symbols = MAX_SYMBOLS;
                for (int i = 0; i < state->num_symbols; i++) {
                    const char *sym = json_object_get_string(json_object_array_get_idx(symbols_array, i));
                    state->symbols[i] = strdup(sym);
                    state->trade_count[i] = 0;
                    state->trade_head[i] = 0;
                    state->last_alert_time[i] = 0;
                }

                pthread_mutex_unlock(&state->symbols_mutex);

                // Trigger re-subscription
                if (state->wsi_finnhub) {
                    FinnhubSession *session = (FinnhubSession *)lws_wsi_user(state->wsi_finnhub);
                    session->sub_index = 0;
                    lws_callback_on_writable(state->wsi_finnhub);
                }
            }

            json_object_put(msg);
            free(msg_str);
            break;
        }

        case LWS_CALLBACK_CLIENT_CLOSED:
            LOG_WS("Local server connection closed\n");
            state->wsi_local = NULL;
            handle_local_server_connection(state);  // Attempt to reconnect
            break;

        default:
            break;
    }
    return 0;
}

static int finnhub_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    FinnhubSession *session = (FinnhubSession *)user;
    ScannerState *state = (ScannerState *)lws_context_user(lws_get_context(wsi));

    LOG_WS("Finnhub callback reason: %d\n", reason);

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            LOG_WS("Connected to FINNHUB\n");
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (session->sub_index < state->num_symbols) {
                LOG_WS("Total symbols to subscribe: %d\n", state->num_symbols);

                char subscribe_msg[128];

                pthread_mutex_lock(&state->symbols_mutex);
                const char *symbol = state->symbols[session->sub_index];
                snprintf(subscribe_msg, sizeof(subscribe_msg), "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", symbol);
                pthread_mutex_unlock(&state->symbols_mutex);

                unsigned char buf[LWS_PRE + 128];
                unsigned char *p = &buf[LWS_PRE];
                size_t msg_len = strlen(subscribe_msg);
                memcpy(p, subscribe_msg, msg_len);
                lws_write(wsi, p, msg_len, LWS_WRITE_TEXT);

                // ⏱ Log the time between subscriptions
                static struct timeval last_time = {0};
                struct timeval now;
                gettimeofday(&now, NULL);

                if (last_time.tv_sec != 0) {
                    long delta_ms = (now.tv_sec - last_time.tv_sec) * 1000 + (now.tv_usec - last_time.tv_usec) / 1000;
                    LOG_WS("🕒 Time since last sub: %ld ms\n", delta_ms);
                } else {
                    LOG_WS("🕒 First subscription\n");
                }

                last_time = now;

                LOG_WS("Subscribed to: %s (%d/%d)\n", symbol, session->sub_index + 1, state->num_symbols);
                session->sub_index++;

                if (session->sub_index < state->num_symbols) {
                    lws_set_timer_usecs(wsi, 250 * 1000);  // 250ms delay
                }
            }
            break;

        case LWS_CALLBACK_TIMER:
            lws_callback_on_writable(wsi);  // This triggers the next subscribe
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            // Ignore ping messages
            if (len == 4 && strncmp((char *)in, "ping", 4) == 0) {
                LOG_WS("Ignored ping message.\n");
                break;
            }
            struct json_object *msg = json_tokener_parse((char *)in);
            if (!msg) {
                LOG_WS("Failed to parse JSON message: %.*s\n", (int)len, (char *)in);
                break;
            }
            // If error type, log and ignore
            struct json_object *type_obj;
            if (json_object_object_get_ex(msg, "type", &type_obj)) {
                const char *msg_type = json_object_get_string(type_obj);
                if (strcmp(msg_type, "error") == 0) {
                    LOG_WS("Error message received: %s\n", json_object_to_json_string(msg));
                    json_object_put(msg);
                    break;
                }
            }
            // Process the "data" array of trades
            struct json_object *data_array;
            if (!json_object_object_get_ex(msg, "data", &data_array) || json_object_get_type(data_array) != json_type_array) {
                LOG_WS("JSON does not contain a valid 'data' array. Ignoring message.\n");
                json_object_put(msg);
                break;
            }
            int arr_len = json_object_array_length(data_array);
            for (int i = 0; i < arr_len; i++) {
                struct json_object *trade_obj = json_object_array_get_idx(data_array, i);
                struct json_object *sym_obj, *price_obj, *vol_obj;
                if (json_object_object_get_ex(trade_obj, "s", &sym_obj) && json_object_object_get_ex(trade_obj, "p", &price_obj) && json_object_object_get_ex(trade_obj, "v", &vol_obj)) {
                    const char *symbol = json_object_get_string(sym_obj);
                    double price = json_object_get_double(price_obj);
                    int volume = json_object_get_int(vol_obj);
                    LOG_WS("Received trade: symbol=%s, price=%.2f, volume=%d\n", symbol, price, volume);
                    enqueue_trade(state, symbol, price, volume);
                } else {
                    LOG_WS("Missing required trade data fields. Skipping entry.\n");
                }
            }
            json_object_put(msg);
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            LOG_WS("Finnhub connection error: %s\n", in ? (char *)in : "Unknown error");
            handle_finnhub_connection(state);
            break;

        case LWS_CALLBACK_CLOSED:
            LOG_WS("Finnhub connection closed\n");
            handle_finnhub_connection(state);
            break;

        default:
            break;
    }
    return 0;
}

/* ----------------------------- Worker Threads ----------------------------- */

DWORD WINAPI trade_processing_thread(LPVOID lpParam) {
    ScannerState *state = (ScannerState *)lpParam;

    while (!state->shutdown_flag) {
        TradeMsg trade;
        pthread_mutex_lock(&state->trade_queue.mutex);

        while (trade_queue_empty(&state->trade_queue) && !state->shutdown_flag) {
            pthread_cond_wait(&state->trade_queue.cond, &state->trade_queue.mutex);
        }

        if (state->shutdown_flag) {
            pthread_mutex_unlock(&state->trade_queue.mutex);
            break;
        }

        queue_pop_trade(&state->trade_queue, &trade);
        pthread_mutex_unlock(&state->trade_queue.mutex);

        // Ignore individual trades below the minimum trade volume
        if (trade.volume < MIN_TRADE_VOLUME) {
            LOG_PROCESSING("Ignoring trade: %s | Price: %.2f | Volume: %d (Below threshold: %d)\n", trade.symbol, trade.price, trade.volume, MIN_TRADE_VOLUME);
            continue;
        }

        // Find the symbol index
        int idx = -1;
        pthread_mutex_lock(&state->symbols_mutex);
        for (int i = 0; i < state->num_symbols; i++) {
            if (strcmp(state->symbols[i], trade.symbol) == 0) {
                idx = i;
                break;
            }
        }
        pthread_mutex_unlock(&state->symbols_mutex);

        if (idx < 0) {
            LOG_PROCESSING("Symbol %s not found in tracked symbols\n", trade.symbol);
            continue;
        }

        pthread_mutex_lock(&state->symbols_mutex);
        uint64_t current_time = get_current_time_ms();

        LOG_PROCESSING("Received trade: %s | Price: %.2f | Volume: %d | Timestamp: %llu\n", trade.symbol, trade.price, trade.volume, current_time);

        // Store trade in circular buffer
        int pos = (state->trade_head[idx] + state->trade_count[idx]) % MAX_TRADES;
        state->trade_history[idx][pos].price = trade.price;
        state->trade_history[idx][pos].timestamp = current_time;
        state->trade_history[idx][pos].volume = trade.volume;

        // ✅ Update trade count & handle buffer full condition
        if (state->trade_count[idx] < MAX_TRADES) {
            state->trade_count[idx]++;
        } else {
            // ✅ If buffer is full, overwrite oldest trade
            state->trade_head[idx] = (state->trade_head[idx] + 1) % MAX_TRADES;
            LOG_PROCESSING("Buffer full, overwriting oldest trade at index %d\n", state->trade_head[idx]);
        }

        // ✅ Compute rolling 5-minute volume AFTER storing the latest trade
        uint64_t five_min_ago = current_time - 300000;  // 5 minutes ago
        uint64_t rolling_5min_volume = 0;

        for (int i = 0; i < state->trade_count[idx]; i++) {
            int trade_pos = (state->trade_head[idx] + i) % MAX_TRADES;
            if (state->trade_history[idx][trade_pos].timestamp >= five_min_ago) {
                rolling_5min_volume += state->trade_history[idx][trade_pos].volume;
            }
        }

        // ✅ Debug log to verify rolling volume calculation
        LOG_PROCESSING("Rolling 5-min volume for %s: %lu\n", trade.symbol, rolling_5min_volume);

        // Find the price 5 minutes ago
        double old_price = 0.0;
        uint64_t oldest_time = 0;
        for (int i = 0; i < state->trade_count[idx]; i++) {
            int trade_pos = (state->trade_head[idx] + i) % MAX_TRADES;
            if (state->trade_history[idx][trade_pos].timestamp >= five_min_ago) {
                old_price = state->trade_history[idx][trade_pos].price;
                oldest_time = state->trade_history[idx][trade_pos].timestamp;
                break;  // ✅ Stops at the first valid trade in the 5-min window
            }
        }

        // Initialize last_alert_price if first trade for this symbol
        if (state->last_alert_price[idx] == 0.0) {
            state->last_alert_price[idx] = trade.price;
            LOG_PROCESSING("Initialized last_alert_price for %s to %.2f\n", trade.symbol, trade.price);
        }

        if (old_price > 0) {
            double change = ((trade.price - old_price) / old_price) * 100.0;

            LOG_PROCESSING("%s | Old Price: %.2f -> New Price: %.2f | Change: %.2f%%\n", trade.symbol, old_price, trade.price, change);

            int should_alert = 0;

            // Allow first alert without requiring a price breakout
            if ((change > 0 && trade.price > state->last_alert_price[idx]) || (change < 0 && trade.price < state->last_alert_price[idx])) {
                should_alert = 1;
            }

            if (should_alert && fabs(change) >= PRICE_MOVEMENT && (current_time - state->last_alert_time[idx] >= DEBOUNCE_TIME) &&
                rolling_5min_volume >= MIN_CUMULATIVE_VOLUME) {  // ✅ Use rolling 5-min volume

                AlertMsg alert;
                alert.symbol_index = idx;
                alert.change = change;
                alert.price = trade.price;
                alert.volume = rolling_5min_volume;

                pthread_mutex_lock(&state->alert_queue.mutex);
                queue_push_alert(&state->alert_queue, &alert);
                pthread_cond_signal(&state->alert_queue.cond);
                pthread_mutex_unlock(&state->alert_queue.mutex);

                LOG_PROCESSING("ALERT TRIGGERED for %s | Change: %.2f%% | 5-min Rolling Volume: %lu\n", trade.symbol, change, rolling_5min_volume);

                state->last_alert_time[idx] = current_time;
                state->last_alert_price[idx] = trade.price;
            } else {
                LOG_PROCESSING(
                    "🚨 ALERT **NOT** TRIGGERED for %s | Change: %.2f%% | "
                    "5-min Rolling Volume: %lu / %d | Time Since Last Alert: %llu ms\n",
                    trade.symbol, change, rolling_5min_volume, MIN_CUMULATIVE_VOLUME, (current_time - state->last_alert_time[idx]));

                if (!should_alert) LOG_PROCESSING("  * should_alert condition failed. Last alert price: %.2f, Current price: %.2f\n", state->last_alert_price[idx], trade.price);
                if (fabs(change) < PRICE_MOVEMENT) LOG_PROCESSING("  * Change %.2f%% is below threshold %.2f%%\n", fabs(change), PRICE_MOVEMENT);
                if ((current_time - state->last_alert_time[idx]) < DEBOUNCE_TIME)
                    LOG_PROCESSING("  * Debounce time not met. Time since last alert: %llu ms (Debounce required: %d ms)\n", (current_time - state->last_alert_time[idx]), DEBOUNCE_TIME);
                if (rolling_5min_volume < MIN_CUMULATIVE_VOLUME) LOG_PROCESSING("  * 5-min Rolling Volume %lu is below threshold %d\n", rolling_5min_volume, MIN_CUMULATIVE_VOLUME);
            }
        }

        pthread_mutex_unlock(&state->symbols_mutex);
    }
    return 0;
}

DWORD WINAPI trade_cleaner_thread(LPVOID lpParam) {
    ScannerState *state = (ScannerState *)lpParam;

    while (!state->shutdown_flag) {
        uint64_t current_time = get_current_time_ms();

        pthread_mutex_lock(&state->symbols_mutex);
        for (int i = 0; i < state->num_symbols; i++) {
            int removed_count = 0;

            while (state->trade_count[i] > 0) {
                int oldest_pos = state->trade_head[i];

                uint64_t trade_time = state->trade_history[i][oldest_pos].timestamp;

                if (current_time - trade_time >= 300000) {  // 5 minutes = 300000 ms
                    // Remove trade from circular buffer
                    state->trade_head[i] = (state->trade_head[i] + 1) % MAX_TRADES;
                    state->trade_count[i]--;
                    removed_count++;
                } else {
                    break;  // Stop if we reach a trade within the 5-minute window
                }
            }

            // ✅ Log after the while loop (but only if something was removed)
            if (removed_count > 0) {
                LOG_CLEANER("[trade_cleaner_thread] Symbol: %s | Removed Trades: %d\n", state->symbols[i], removed_count);
            }
        }
        pthread_mutex_unlock(&state->symbols_mutex);

        SLEEP_MS(1000);  // Run cleanup every second
    }
    return 0;
}

void print_trade_history(ScannerState *state, const char *symbol) {
    pthread_mutex_lock(&state->symbols_mutex);
    int idx = -1;

    for (int i = 0; i < state->num_symbols; i++) {
        if (strcmp(state->symbols[i], symbol) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        LOG("[print_trade_history] Symbol %s not found\n", symbol);
        pthread_mutex_unlock(&state->symbols_mutex);
        return;
    }

    LOG("[print_trade_history] Trade history for %s:\n", symbol);
    for (int j = 0; j < state->trade_count[idx]; j++) {
        LOG("Trade %d | Price: %.2f | Timestamp: %llu\n", j, state->trade_history[idx][j].price, state->trade_history[idx][j].timestamp);
    }

    pthread_mutex_unlock(&state->symbols_mutex);
}

// Alert sending thread: reads alerts from the alert queue and sends them.
DWORD WINAPI alert_sending_thread(LPVOID lpParam) {
    ScannerState *state = (ScannerState *)lpParam;

    while (!state->shutdown_flag) {
        AlertMsg alert;

        pthread_mutex_lock(&state->alert_queue.mutex);
        while (alert_queue_empty(&state->alert_queue) && !state->shutdown_flag) {
            pthread_cond_wait(&state->alert_queue.cond, &state->alert_queue.mutex);
        }

        if (state->shutdown_flag) {
            pthread_mutex_unlock(&state->alert_queue.mutex);
            break;
        }

        queue_pop_alert(&state->alert_queue, &alert);
        pthread_mutex_unlock(&state->alert_queue.mutex);

        send_alert(state, alert.symbol_index, alert.change, alert.price, alert.volume);
    }

    return 0;
}

THREAD_FUNC connection_watchdog_thread(void *arg) {
    ScannerState *state = (ScannerState *)arg;

    while (!state->shutdown_flag) {
        unsigned long now = get_current_time_ms();

        // ✅ Sanity check for valid timestamp
        if (state->last_ping_time > 0 && (now - state->last_ping_time) > 60000) {
            LOG_WS("⚠️ No ping received in 60s, rebooting program...\n");

            // ✅ Properly close connection before exiting
            if (state->wsi_local) {
                lws_set_timeout(state->wsi_local, 1, LWS_TO_KILL_ASYNC);
                state->wsi_local = NULL;
            }

            state->last_ping_time = get_current_time_ms();  // Reset timer

            // ✅ Ensure thread cleanup happens (if needed)
            state->shutdown_flag = 1;

            // ✅ Exit AFTER cleanup
            exit(42);  // Unique exit code for reboot
        }

        SLEEP_MS(5000);
    }
    return 0;
}

/* ----------------------------- Signal Handling ----------------------------- */
volatile sig_atomic_t shutdown_flag = 0;
volatile sig_atomic_t restart_flag = 0;

void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        shutdown_flag = 1;
    } else if (sig == SIGSEGV || sig == SIGABRT) {
        restart_flag = 1;
    }
}

/* ----------------------------- Main Function ----------------------------- */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s {scanner_id}\n", argv[0]);
        return 1;
    }

    const char *scanner_id = argv[1];

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGSEGV, handle_signal);
    signal(SIGABRT, handle_signal);

    ScannerState state;  // ✅ Allocate once
    initialize_state(&state);

    strncpy(state.scanner_id, scanner_id, sizeof(state.scanner_id) - 1);
    state.scanner_id[sizeof(state.scanner_id) - 1] = '\0';

    struct lws_protocols protocols[] = {{"local-server", local_server_callback, 0, 0}, {"finnhub", finnhub_callback, sizeof(FinnhubSession), 0}, {NULL, NULL, 0, 0}};

    struct lws_context_creation_info info = {0};
    info.protocols = protocols;
    info.user = &state;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    state.context = lws_create_context(&info);
    if (!state.context) {
        LOG("lws_create_context failed\n");
        cleanup_state(&state);
        return -1;
    }

    LOG("Scanner ID: %s\n", scanner_id);

    handle_local_server_connection(&state);
    handle_finnhub_connection(&state);

#ifdef _WIN32
    HANDLE hTradeThread = CreateThread(NULL, 0, trade_processing_thread, &state, 0, NULL);
    HANDLE hAlertThread = CreateThread(NULL, 0, alert_sending_thread, &state, 0, NULL);
    HANDLE hCleanerThread = CreateThread(NULL, 0, trade_cleaner_thread, &state, 0, NULL);
    HANDLE hWatchdogThread = CreateThread(NULL, 0, connection_watchdog_thread, &state, 0, NULL);
#else
    pthread_t hTradeThread, hAlertThread, hCleanerThread, hWatchdogThread;
    pthread_create(&hTradeThread, NULL, trade_processing_thread, &state);
    pthread_create(&hAlertThread, NULL, alert_sending_thread, &state);
    pthread_create(&hCleanerThread, NULL, trade_cleaner_thread, &state);
    pthread_create(&hWatchdogThread, NULL, connection_watchdog_thread, &state);
#endif

    // ✅ Main LWS loop
    while (!shutdown_flag && !restart_flag) {
        lws_service(state.context, 50);  // Keep LWS timers and events running
    }

    // ✅ Signal threads to shut down
    state.shutdown_flag = 1;
    pthread_cond_broadcast(&state.trade_queue.cond);
    pthread_cond_broadcast(&state.alert_queue.cond);

#ifdef _WIN32
    WaitForSingleObject(hTradeThread, INFINITE);
    WaitForSingleObject(hAlertThread, INFINITE);
    WaitForSingleObject(hCleanerThread, INFINITE);
    WaitForSingleObject(hWatchdogThread, INFINITE);
#else
    pthread_join(hTradeThread, NULL);
    pthread_join(hAlertThread, NULL);
    pthread_join(hCleanerThread, NULL);
    pthread_join(hWatchdogThread, NULL);
#endif

    lws_context_destroy(state.context);
    cleanup_state(&state);

    if (restart_flag) {
        LOG("Restarting program due to crash...\n");
        restart_flag = 0;
        // In a restart scenario, you could add `execv(argv[0], argv);` here.
    }

    LOG("Program shutdown gracefully.\n");
    return 0;
}
