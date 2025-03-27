#include <json-c/json.h>
#include <libwebsockets.h>
#include <libwebsockets/lws-vhost.h>  // for lws_vhost_creation_info
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
#define WS_MODE 1
#define PROCESSING_MODE 1
#define DEBUG_MODE 0  // Disable normal debug logs

#define MAX_SYMBOLS 50
#define PRICE_MOVEMENT 1.0   // 1% price movement
#define DEBOUNCE_TIME 5000   // 3 seconds in milliseconds
#define MAX_TRADES 1000      // Upper limit for active trades to avoid memory overload
#define MAX_QUEUE_SIZE 1024  // For both trade and alert queues

#define LOCAL_PORT 8000
#define FINNHUB_HOST "ws.finnhub.io"

char *LOCAL_ADDRESS = NULL;
char *FINNHUB_PATH = NULL;

void init_config() {
    LOCAL_ADDRESS = getenv("LOCAL_ADDRESS") ? strdup(getenv("LOCAL_ADDRESS")) : strdup("ws://localhost:8000/ws");
    FINNHUB_PATH = getenv("FINNHUB_PATH") ? strdup(getenv("FINNHUB_PATH")) : strdup("/?token=YOURTOKEN");
}

void cleanup_config() {
    free(LOCAL_ADDRESS);
    free(FINNHUB_PATH);
}

#define MIN_TRADE_VOLUME 1           // Ignore individual trades below this volume
#define MIN_CUMULATIVE_VOLUME 10000  // Only trigger alerts if cumulative volume is above this threshold

/* ----------------------------- LOGGING ------------------------------ */
#define LOG(fmt, ...) \
    if (CLEANER_MODE == 0) printf("[%s] " fmt, __func__, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    if (DEBUG_MODE) LOG(fmt, ##__VA_ARGS__)

#define LOG_CLEANER(fmt, ...) \
    if (CLEANER_MODE) printf("[%s] " fmt, __func__, ##__VA_ARGS__)

#define LOG_ALERTS(fmt, ...) \
    if (ALERTS_MODE) printf("[ALERT] [%s] " fmt, __func__, ##__VA_ARGS__)

#define LOG_WS(fmt, ...) \
    if (WS_MODE) printf("[WS] [%s] " fmt, __func__, ##__VA_ARGS__)
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

    // NEW: Store the last alerted price per symbol
    double last_alert_price[MAX_SYMBOLS];

    unsigned long last_ping_time;  // Track last received ping timestamp

    // Trade storage (circular buffer for each symbol)
    struct {
        double price;
        uint64_t timestamp;
        int volume;  // Store volume per trade
    } trade_history[MAX_SYMBOLS][MAX_TRADES];

    int trade_count[MAX_SYMBOLS];             // Tracks the number of stored trades per symbol
    int trade_head[MAX_SYMBOLS];              // Points to the oldest trade in the history
    unsigned long total_volume[MAX_SYMBOLS];  // Tracks cumulative volume in the last 5 min

    struct lws *wsi_local;
    struct lws *wsi_finnhub;
    struct lws_context *context;

    TradeQueue trade_queue;
    AlertQueue alert_queue;

    pthread_mutex_t symbols_mutex;
    volatile int shutdown_flag;
    char scanner_id[64];

    volatile int subscriptions_complete;
    unsigned long last_symbol_update_time;

} ScannerState;

// Session data for Finnhub connection
typedef struct {
    int sub_index;
} FinnhubSession;

LOG_WS("LWS version: %s", lws_get_library_version());

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
static struct lws_protocols protocols[] = {
    {.name = "finnhub", .callback = finnhub_callback, .per_session_data_size = sizeof(FinnhubSession), .rx_buffer_size = 4096},
    {.name = "local-server", .callback = local_server_callback, .per_session_data_size = 0, .rx_buffer_size = 1024},
    {NULL, NULL, 0, 0}  // terminator
};

static void initialize_state(ScannerState *state) {
    memset(state, 0, sizeof(*state));
    pthread_mutex_init(&state->symbols_mutex, NULL);
    pthread_mutex_init(&state->trade_queue.mutex, NULL);
    pthread_cond_init(&state->trade_queue.cond, NULL);
    pthread_mutex_init(&state->alert_queue.mutex, NULL);
    pthread_cond_init(&state->alert_queue.cond, NULL);

    state->last_ping_time = get_current_time_ms();
    state->last_symbol_update_time = get_current_time_ms();

    struct lws_context_creation_info info = {0};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    state->context = lws_create_context(&info);
    if (!state->context) {
        LOG_WS("❌ Failed to create LWS context!");
        exit(1);
    } else {
        LOG_WS("✅ LWS context created — timers should now work");
    }
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

/* ----------------------------- WebSocket Handlers
 * ----------------------------- */
static int handle_local_server_connection(ScannerState *state) {
    if (!state || !state->context) {
        LOG_WS("Invalid state or context when connecting to local server\n");
        return -1;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = state->context;
    ccinfo.address = LOCAL_ADDRESS;
    ccinfo.port = LOCAL_PORT;
    ccinfo.path = "/ws";
    ccinfo.host = LOCAL_ADDRESS;
    ccinfo.origin = LOCAL_ADDRESS;
    ccinfo.protocol = "local-server";
    ccinfo.ssl_connection = 0;

    state->wsi_local = lws_client_connect_via_info(&ccinfo);
    if (!state->wsi_local) {
        LOG_WS("Failed to initiateo local server\n");
        return -1;
    }

    return 0;
}
static int handle_finnhub_connection(ScannerState *state) {
    if (!state || !state->context) {
        LOG_WS("Invalid state or context when connecting to Finnhub\n");
        return -1;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = state->context;
    ccinfo.address = FINNHUB_HOST;
    ccinfo.port = 443;
    ccinfo.path = FINNHUB_PATH;
    ccinfo.host = FINNHUB_HOST;
    ccinfo.origin = FINNHUB_HOST;
    ccinfo.protocol = "finnhub";
    ccinfo.ssl_connection = LCCSCF_USE_SSL;

    state->wsi_finnhub = lws_client_connect_via_info(&ccinfo);
    if (!state->wsi_finnhub) {
        LOG_WS(
            "Failed to initiate Finnhub connection: host=%s, path=%s, port=%d, "
            "errno=%d (%s)\n",
            FINNHUB_HOST, FINNHUB_PATH, ccinfo.port, errno, strerror(errno));
        return -1;
    }

    LOG_WS("Finnhub connection initiated successfully\n");
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

/* ----------------------------- WebSocket Callbacks
 * ----------------------------- */
static int local_server_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    ScannerState *state = (ScannerState *)lws_context_user(lws_get_context(wsi));

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            LOG_WS("Connected to local server\n");
            state->wsi_local = wsi;

            // ✅ Initialize ping timer on connection
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
            char *msg_str = malloc(len + 1);
            if (!msg_str) {
                LOG_WS("❌ Memory allocation failed for msg_str");
                break;
            }
            strncpy(msg_str, (char *)in, len);
            msg_str[len] = '\0';

            struct json_object *root = json_tokener_parse(msg_str);
            if (!root) {
                LOG_WS("❌ Failed to parse JSON message: %s", msg_str);
                free(msg_str);
                break;
            }

            struct json_object *type_obj;
            if (json_object_object_get_ex(root, "type", &type_obj)) {
                const char *msg_type = json_object_get_string(type_obj);

                if (strcmp(msg_type, "ping") == 0) {
                    // ✅ Update ping timestamp on reception
                    state->last_ping_time = get_current_time_ms();

                    static unsigned long last_pong_time = 0;
                    unsigned long now = get_current_time_ms();

                    if (now - last_pong_time > 1000) {
                        char pong_buffer[128];
                        int pong_len = snprintf(pong_buffer, sizeof(pong_buffer), "{\"type\":\"pong\",\"client_id\":\"%s\"}", state->scanner_id);

                        if (pong_len < sizeof(pong_buffer)) {
                            unsigned char buf[LWS_PRE + pong_len];
                            unsigned char *p = &buf[LWS_PRE];
                            memcpy(p, pong_buffer, pong_len);

                            if (lws_write(wsi, p, pong_len, LWS_WRITE_TEXT) < 0) {
                                LOG_WS("❌ Failed to send Pong message.");
                            } else {
                                LOG_WS("✅ Pong sent: %s", pong_buffer);
                                last_pong_time = now;
                            }
                        } else {
                            LOG_WS("❌ Pong buffer overflow detected!");
                        }
                    }

                    json_object_put(root);
                    free(msg_str);
                    break;
                }
            }

            json_object_put(root);

            // ✅ Parse the message again to check for symbols
            struct json_object *msg = json_tokener_parse(msg_str);
            struct json_object *symbols_array;
            if (json_object_object_get_ex(msg, "symbols", &symbols_array)) {
                pthread_mutex_lock(&state->symbols_mutex);

                // ✅ Log received symbols
                LOG_WS("Received %d symbols to subscribe", json_object_array_length(symbols_array));

                if (state->wsi_finnhub) {
                    for (int i = 0; i < state->num_symbols; i++) {
                        char unsubscribe_msg[128];
                        snprintf(unsubscribe_msg, sizeof(unsubscribe_msg), "{\"type\":\"unsubscribe\",\"symbol\":\"%s\"}", state->symbols[i]);

                        unsigned char buf[LWS_PRE + 128];
                        unsigned char *p = &buf[LWS_PRE];
                        size_t msg_len = strlen(unsubscribe_msg);
                        memcpy(p, unsubscribe_msg, msg_len);

                        if (lws_write(state->wsi_finnhub, p, msg_len, LWS_WRITE_TEXT) < 0) {
                            LOG_WS("❌ Failed to unsubscribe from: %s", unsubscribe_msg);
                        } else {
                            LOG_WS("✅ Unsubscribed from: %s", unsubscribe_msg);
                        }
                    }
                }

                // ✅ Free old symbols safely
                for (int i = 0; i < state->num_symbols; i++) {
                    free(state->symbols[i]);
                    state->symbols[i] = NULL;
                }

                // ✅ Update symbols list
                state->num_symbols = json_object_array_length(symbols_array);
                if (state->num_symbols > MAX_SYMBOLS) state->num_symbols = MAX_SYMBOLS;
                LOG_WS("🔄 [%s] Scanner received %d symbols to subscribe", state->scanner_id, state->num_symbols);

                // ✅ Mark the time we received new symbols (for watchdog grace period)
                state->last_symbol_update_time = get_current_time_ms();
                LOG_WS("⏱️ Updated last_symbol_update_time: %lu\n", state->last_symbol_update_time);

                for (int i = 0; i < state->num_symbols; i++) {
                    const char *sym = json_object_get_string(json_object_array_get_idx(symbols_array, i));
                    if (sym) {
                        state->symbols[i] = strdup(sym);
                        LOG_WS("✅ Added new symbol: %s", state->symbols[i]);
                    } else {
                        LOG_WS("❌ Failed to retrieve symbol string from JSON");
                    }
                }

                pthread_mutex_unlock(&state->symbols_mutex);

                // ✅ Trigger re-subscription on Finnhub
                if (state->wsi_finnhub) {
                    FinnhubSession *session = (FinnhubSession *)lws_wsi_user(state->wsi_finnhub);
                    session->sub_index = 0;
                    lws_set_timer_usecs(state->wsi_finnhub, 1);  // Trigger the timer immediately
                    LOG_WS("🟢 [%s] Restarted subscription timer for new symbols", state->scanner_id);
                }
            }

            json_object_put(msg);
            free(msg_str);
            break;
        }

        case LWS_CALLBACK_CLIENT_CLOSED:
            LOG_WS("Local server connection closed\n");
            state->wsi_local = NULL;
            handle_local_server_connection(state);
            break;

        default:
            break;
    }
    return 0;
}

#define FINNHUB_LOG_MODE 1

static int finnhub_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    FinnhubSession *session = (FinnhubSession *)user;
    ScannerState *state = (ScannerState *)lws_context_user(lws_get_context(wsi));

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            LOG_WS("Connected to Finnhub");
            // Start timer loop after connect
            lws_set_timer_usecs(wsi, 1);  // start immediately
            break;

        case LWS_CALLBACK_TIMER:
            if (session->sub_index < state->num_symbols) {
                pthread_mutex_lock(&state->symbols_mutex);

                const char *symbol = state->symbols[session->sub_index];
                char subscribe_msg[128];
                snprintf(subscribe_msg, sizeof(subscribe_msg), "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", symbol);

                LOG_WS("📡 [%s] Subscribing to symbol %d/%d: %s", state->scanner_id, session->sub_index + 1, state->num_symbols, symbol);

                unsigned char buf[LWS_PRE + 128];
                unsigned char *p = &buf[LWS_PRE];
                size_t msg_len = strlen(subscribe_msg);
                memcpy(p, subscribe_msg, msg_len);
                lws_write(wsi, p, msg_len, LWS_WRITE_TEXT);

                session->sub_index++;
                pthread_mutex_unlock(&state->symbols_mutex);

                session->sub_index++;
                if (session->sub_index < state->num_symbols) {
                    lws_set_timer_usecs(wsi, 200000);  // Schedule next in 200ms
                } else {
                    state->subscriptions_complete = 1;
                    LOG_WS("✅ All subscriptions complete, watchdog is now active");
                }
            }
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            // Leave this empty now or just log
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

void *trade_processing_thread(void *lpParam) {
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
            LOG_PROCESSING(
                "[trade_processing_thread] Ignoring trade: %s | Price: %.2f | "
                "Volume: %d (Below threshold: %d)\n",
                trade.symbol, trade.price, trade.volume, MIN_TRADE_VOLUME);
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
            LOG_PROCESSING("[trade_processing_thread] Symbol %s not found in tracked symbols\n", trade.symbol);
            continue;
        }

        pthread_mutex_lock(&state->symbols_mutex);
        uint64_t current_time = get_current_time_ms();

        LOG_PROCESSING(
            "[trade_processing_thread] Received trade: %s | Price: %.2f | "
            "Volume: %d | Timestamp: %llu\n",
            trade.symbol, trade.price, trade.volume, current_time);

        // Store trade in circular buffer
        int pos = (state->trade_head[idx] + state->trade_count[idx]) % MAX_TRADES;
        state->trade_history[idx][pos].price = trade.price;
        state->trade_history[idx][pos].timestamp = current_time;
        state->trade_history[idx][pos].volume = trade.volume;

        // ✅ Remove cumulative volume tracking (compute dynamically)
        uint64_t five_min_ago = current_time - 300000;  // 5 minutes ago

        // ✅ Compute rolling 5-minute volume
        uint64_t rolling_5min_volume = 0;
        for (int i = 0; i < state->trade_count[idx]; i++) {
            int trade_pos = (state->trade_head[idx] + i) % MAX_TRADES;  // ✅ Correct indexing
            if (state->trade_history[idx][trade_pos].timestamp >= five_min_ago) {
                rolling_5min_volume += state->trade_history[idx][trade_pos].volume;
            }
        }

        // ✅ Log every minute
        static uint64_t last_log_time = 0;
        if (current_time - last_log_time >= 300000) {  // Every 5 min
            LOG_PROCESSING("DEBUG: %s | 5-min Rolling Volume: %lu | Last Trade: %.2f\n", trade.symbol, rolling_5min_volume, trade.price);
            last_log_time = current_time;  // ✅ Update timestamp
        }

        // ✅ Update trade count & handle buffer full condition
        if (state->trade_count[idx] < MAX_TRADES) {
            state->trade_count[idx]++;
        } else {
            // ✅ If buffer is full, overwrite oldest trade
            state->trade_head[idx] = (state->trade_head[idx] + 1) % MAX_TRADES;
            LOG_PROCESSING("Buffer full, overwriting oldest trade at index %d\n", state->trade_head[idx]);
        }

        // ✅ Find the correct 5-min-ago price
        double old_price = 0.0;
        uint64_t oldest_time = 0;
        for (int i = 0; i < state->trade_count[idx]; i++) {
            int trade_pos = (state->trade_head[idx] + i) % MAX_TRADES;
            if (state->trade_history[idx][trade_pos].timestamp >= five_min_ago) {
                old_price = state->trade_history[idx][trade_pos].price;
                oldest_time = state->trade_history[idx][trade_pos].timestamp;
                break;
            }
        }

        // If this is the first recorded trade for this symbol, initialize
        // last_alert_price
        if (state->last_alert_price[idx] == 0.0) {
            state->last_alert_price[idx] = trade.price;
            LOG_PROCESSING(
                "[trade_processing_thread] Initialized last_alert_price for %s "
                "to %.2f\n",
                trade.symbol, trade.price);
        }

        if (old_price > 0) {  // ✅ Now old_price is correctly retrieved
            double change = ((trade.price - old_price) / old_price) * 100.0;

            LOG_PROCESSING(
                "[trade_processing_thread] %s | Old Price: %.2f -> New Price: "
                "%.2f | Change: %.2f%%\n",
                trade.symbol, old_price, trade.price, change);

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
                alert.volume = rolling_5min_volume;  // ✅ Use rolling 5-min volume

                pthread_mutex_lock(&state->alert_queue.mutex);
                queue_push_alert(&state->alert_queue, &alert);
                pthread_cond_signal(&state->alert_queue.cond);
                pthread_mutex_unlock(&state->alert_queue.mutex);

                LOG_PROCESSING(
                    "[trade_processing_thread] ALERT TRIGGERED for %s | "
                    "Change: %.2f%% | 5-min Rolling Volume: %lu\n",
                    trade.symbol, change, rolling_5min_volume);

                state->last_alert_time[idx] = current_time;
                state->last_alert_price[idx] = trade.price;  // ✅ Store last alerted price

            } else {
                LOG_PROCESSING(
                    "[trade_processing_thread] 🔍 ALERT **NOT** TRIGGERED for %s | "
                    "Change: %.2f%% | "
                    "5-min Rolling Volume: %lu / %d | Time Since Last Alert: %llu ms\n",
                    trade.symbol, change, rolling_5min_volume, MIN_CUMULATIVE_VOLUME, (current_time - state->last_alert_time[idx]));

                // Additional logging for each failed condition:
                if (!should_alert)
                    LOG_PROCESSING(
                        "[trade_processing_thread] * should_alert condition "
                        "failed. Last alert price: %.2f, Current price: %.2f\n",
                        state->last_alert_price[idx], trade.price);
                if (fabs(change) < PRICE_MOVEMENT)
                    LOG_PROCESSING(
                        "[trade_processing_thread] * Change %.2f%% is "
                        "below threshold %.2f%%\n",
                        fabs(change), PRICE_MOVEMENT);
                if ((current_time - state->last_alert_time[idx]) < DEBOUNCE_TIME)
                    LOG_PROCESSING(
                        "[trade_processing_thread] * Debounce time not met. "
                        "Time since last alert: %llu ms (Debounce required: %d ms)\n",
                        (current_time - state->last_alert_time[idx]), DEBOUNCE_TIME);
                if (rolling_5min_volume < MIN_CUMULATIVE_VOLUME)
                    LOG_PROCESSING(
                        "[trade_processing_thread] * 5-min Rolling Volume "
                        "%lu is below threshold %d\n",
                        rolling_5min_volume, MIN_CUMULATIVE_VOLUME);
            }
        }

        pthread_mutex_unlock(&state->symbols_mutex);  // Ensure unlock happens here
    }
    return 0;
}

void *trade_cleaner_thread(void *lpParam) {
    ScannerState *state = (ScannerState *)lpParam;

    while (!state->shutdown_flag) {
        uint64_t current_time = get_current_time_ms();

        pthread_mutex_lock(&state->symbols_mutex);
        for (int i = 0; i < state->num_symbols; i++) {
            int removed_count = 0;

            while (state->trade_count[i] > 0) {
                int oldest_pos = state->trade_head[i];

                if (current_time - state->trade_history[i][oldest_pos].timestamp >= 300000) {
                    // Remove old trade
                    state->trade_head[i] = (state->trade_head[i] + 1) % MAX_TRADES;
                    state->trade_count[i]--;
                    removed_count++;
                } else {
                    break;  // Stop if trade is within the valid 5-minute window
                }
            }

            if (removed_count > 0) {
                LOG_CLEANER("[trade_cleaner_thread] %s | Removed Trades: %d\n", state->symbols[i], removed_count);
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
void *alert_sending_thread(void *lpParam) {
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
        if (!state->subscriptions_complete) {
            SLEEP_MS(1000);  // Delay watchdog until subs are done
            continue;
        }

        // Grace period: ignore missing pings for 20 seconds after receiving new symbols
        if ((now - state->last_symbol_update_time) < 20000) {
            LOG_WS("⏳ Grace period active, skipping watchdog check\n");
        } else if (state->last_ping_time > 0 && (now - state->last_ping_time) > 60000) {
            LOG_WS("⚠️ No ping received in 60s (no grace period), rebooting program...\n");
            exit(42);
        }

        SLEEP_MS(5000);
    }
    return 0;
}

/* ----------------------------- Signal Handling -----------------------------
 */
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

    init_config();
    printf("Using LOCAL_ADDRESS: %s\n", LOCAL_ADDRESS);
    printf("Using FINNHUB_PATH: %s\n", FINNHUB_PATH);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGSEGV, handle_signal);
    signal(SIGABRT, handle_signal);

    while (!shutdown_flag) {
        ScannerState state;
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

        while (!shutdown_flag && !restart_flag) {
            lws_service(state.context, 50);
            LOG_DEBUG("Running WebSocket event loop\n");
        }

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
        }

        LOG("Program shutdown gracefully.\n");
        return 0;
    }
}
