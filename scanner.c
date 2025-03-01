#include <json-c/json.h>
#include <libwebsockets.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
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
#define DEBUG_MODE 1
#define MAX_SYMBOLS 50
#define PRICE_MOVEMENT 1.0  // 1% price movement
#define DEBOUNCE_TIME 3000  // 3 seconds in milliseconds
#define LOCAL_SERVER_URI "ws://192.168.1.17:8000/ws"
#define FINNHUB_URI "wss://ws.finnhub.io/?token=your_token"
#define MAX_QUEUE_SIZE 1024  // For both trade and alert queues

/* ----------------------------- Helper Macros ------------------------------ */
#define LOG(fmt, ...) printf("[%s] " fmt, __func__, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) \
    if (DEBUG_MODE) LOG(fmt, ##__VA_ARGS__)

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

// Global scanner state; note that we separate the symbols data mutex because
// symbols may be updated by the local server callback.
typedef struct {
    char *symbols[MAX_SYMBOLS];
    int num_symbols;
    double last_checked_price[MAX_SYMBOLS];
    unsigned long last_alert_time[MAX_SYMBOLS];

    // WebSocket connection handles
    struct lws *wsi_local;
    struct lws *wsi_finnhub;
    struct lws_context *context;

    // Queues for inter-thread communication
    TradeQueue trade_queue;
    AlertQueue alert_queue;

    // Mutex for protecting symbols array updates
    pthread_mutex_t symbols_mutex;

    // Global shutdown flag
    volatile int shutdown_flag;
    char scanner_id[64];  // Store scanner ID
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
        LOG("Invalid state or context\n");
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
        LOG("Failed to connect to local server\n");
        return -1;
    }

    LOG("Local server connection initiated\n");
    return 0;
}

static int handle_finnhub_connection(ScannerState *state) {
    if (!state || !state->context) {
        LOG("Invalid state or context\n");
        return -1;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = state->context;
    ccinfo.address = "ws.finnhub.io";
    ccinfo.port = 443;
    ccinfo.path = "/?token=crhlrm9r01qjv9rl4bhgcrhlrm9r01qjv9rl4bi0";  // Replace with your token
    ccinfo.host = ccinfo.address;
    ccinfo.origin = ccinfo.address;
    ccinfo.protocol = "finnhub";
    ccinfo.ssl_connection = LCCSCF_USE_SSL;

    state->wsi_finnhub = lws_client_connect_via_info(&ccinfo);
    if (!state->wsi_finnhub) {
        LOG("Failed to connect to Finnhub\n");
        return -1;
    }

    LOG("Finnhub connection initiated\n");
    return 0;
}

/* ----------------------------- Alert Sending ----------------------------- */
static void send_alert(ScannerState *state, int symbol_idx, double change, double price, int volume) {
    if (symbol_idx < 0 || symbol_idx >= state->num_symbols || !state->symbols[symbol_idx]) {
        LOG("Invalid symbol index or null symbol\n");
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
        LOG("Alert sent: %s\n", payload);
    } else {
        LOG("Local server connection unavailable or choked\n");
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
            LOG("Connected to local server\n");
            state->wsi_local = wsi;
            {
                char register_msg[128];
                snprintf(register_msg, sizeof(register_msg), "{\"client_id\":\"%s\"}", state->scanner_id);
                unsigned char buf[LWS_PRE + 128];
                unsigned char *p = &buf[LWS_PRE];
                size_t msg_len = strlen(register_msg);
                memcpy(p, register_msg, msg_len);
                if (lws_write(wsi, p, msg_len, LWS_WRITE_TEXT) < 0)
                    LOG("Failed to send registration message\n");
                else
                    LOG("Sent registration message: %s\n", register_msg);
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            // Expecting a JSON with a "symbols" array
            struct json_object *msg = json_tokener_parse((char *)in);
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
                        LOG("Unsubscribed from: %s\n", unsubscribe_msg);
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
                    // Initialize price tracking values
                    state->last_checked_price[i] = 0.0;
                    state->last_alert_time[i] = 0;
                }

                pthread_mutex_unlock(&state->symbols_mutex);

                // Trigger re-subscription on Finnhub
                if (state->wsi_finnhub) {
                    FinnhubSession *session = (FinnhubSession *)lws_wsi_user(state->wsi_finnhub);
                    session->sub_index = 0;  // Reset subscription index
                    lws_callback_on_writable(state->wsi_finnhub);
                }
            }
            json_object_put(msg);
            break;
        }

        case LWS_CALLBACK_CLIENT_CLOSED:
            LOG("Local server connection closed\n");
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

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            LOG("Connected to Finnhub\n");
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (session->sub_index < state->num_symbols) {
                char subscribe_msg[128];
                pthread_mutex_lock(&state->symbols_mutex);
                if (session->sub_index < state->num_symbols) {
                    snprintf(subscribe_msg, sizeof(subscribe_msg), "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", state->symbols[session->sub_index]);
                }
                pthread_mutex_unlock(&state->symbols_mutex);
                unsigned char buf[LWS_PRE + 128];
                unsigned char *p = &buf[LWS_PRE];
                size_t msg_len = strlen(subscribe_msg);
                memcpy(p, subscribe_msg, msg_len);
                lws_write(wsi, p, msg_len, LWS_WRITE_TEXT);
                LOG_DEBUG("Subscribed to: %s\n", subscribe_msg);
                LOG("Total symbols subscribed: %d\n", state->num_symbols);
                session->sub_index++;
                if (session->sub_index < state->num_symbols) lws_callback_on_writable(wsi);
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            LOG_DEBUG("Finnhub received data: %.*s\n", (int)len, (char *)in);
            // Ignore ping messages
            if (len == 4 && strncmp((char *)in, "ping", 4) == 0) {
                LOG("Ignored ping message.\n");
                break;
            }
            struct json_object *msg = json_tokener_parse((char *)in);
            if (!msg) {
                LOG("Failed to parse JSON message: %.*s\n", (int)len, (char *)in);
                break;
            }
            // If error type, log and ignore
            struct json_object *type_obj;
            if (json_object_object_get_ex(msg, "type", &type_obj)) {
                const char *msg_type = json_object_get_string(type_obj);
                if (strcmp(msg_type, "error") == 0) {
                    LOG("Error message received: %s\n", json_object_to_json_string(msg));
                    json_object_put(msg);
                    break;
                }
            }
            // Process the "data" array of trades
            struct json_object *data_array;
            if (!json_object_object_get_ex(msg, "data", &data_array) || json_object_get_type(data_array) != json_type_array) {
                LOG("JSON does not contain a valid 'data' array. Ignoring message.\n");
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
                    LOG_DEBUG("Received trade: symbol=%s, price=%.2f, volume=%d\n", symbol, price, volume);
                    enqueue_trade(state, symbol, price, volume);
                } else {
                    LOG("Missing required trade data fields. Skipping entry.\n");
                }
            }
            json_object_put(msg);
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            LOG("Finnhub connection error: %s\n", in ? (char *)in : "Unknown error");
            handle_finnhub_connection(state);
            break;

        case LWS_CALLBACK_CLOSED:
            LOG("Finnhub connection closed\n");
            handle_finnhub_connection(state);
            break;

        default:
            break;
    }
    return 0;
}

/* ----------------------------- Worker Threads ----------------------------- */

// Trade processing thread: reads trades from the trade queue and checks for alerts.
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

        // Find the symbol index (protect access to symbols)
        int idx = -1;
        pthread_mutex_lock(&state->symbols_mutex);
        for (int i = 0; i < state->num_symbols; i++) {
            if (strcmp(state->symbols[i], trade.symbol) == 0) {
                idx = i;
                break;
            }
        }
        pthread_mutex_unlock(&state->symbols_mutex);

        if (idx < 0) continue;  // symbol not found

        // Protect access to last_checked_price and last_alert_time
        pthread_mutex_lock(&state->symbols_mutex);
        double old_price = state->last_checked_price[idx];
        if (old_price <= 0 || isnan(old_price) || isinf(old_price)) {
            state->last_checked_price[idx] = trade.price;
            pthread_mutex_unlock(&state->symbols_mutex);
            continue;
        }

        // Get precise current time in milliseconds
        unsigned long current_time = get_current_time_ms();

        double change = ((trade.price - old_price) / old_price) * 100.0;

        if (fabs(change) >= PRICE_MOVEMENT && (current_time - state->last_alert_time[idx] >= DEBOUNCE_TIME)) {
            AlertMsg alert;
            alert.symbol_index = idx;
            alert.change = change;  // Pass the signed change
            alert.price = trade.price;
            alert.volume = trade.volume;
            pthread_mutex_lock(&state->alert_queue.mutex);
            queue_push_alert(&state->alert_queue, &alert);
            pthread_cond_signal(&state->alert_queue.cond);
            pthread_mutex_unlock(&state->alert_queue.mutex);
            state->last_alert_time[idx] = current_time;
        }

        // Update last checked price
        state->last_checked_price[idx] = trade.price;
        pthread_mutex_unlock(&state->symbols_mutex);
    }
    return 0;
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

        // Send alert using the local websocket connection.
        send_alert(state, alert.symbol_index, alert.change, alert.price, alert.volume);
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

    while (!shutdown_flag) {
        ScannerState state;
        initialize_state(&state);

        // Store the scanner ID
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

        HANDLE hTradeThread = CreateThread(NULL, 0, trade_processing_thread, &state, 0, NULL);
        HANDLE hAlertThread = CreateThread(NULL, 0, alert_sending_thread, &state, 0, NULL);

        while (!shutdown_flag && !restart_flag) {
            lws_service(state.context, 50);
            LOG_DEBUG("Running WebSocket event loop\n");
        }

        state.shutdown_flag = 1;
        pthread_cond_broadcast(&state.trade_queue.cond);
        pthread_cond_broadcast(&state.alert_queue.cond);
        WaitForSingleObject(hTradeThread, INFINITE);
        WaitForSingleObject(hAlertThread, INFINITE);

        lws_context_destroy(state.context);
        cleanup_state(&state);

        if (restart_flag) {
            LOG("Restarting program due to crash...\n");
            restart_flag = 0;
        }
    }

    LOG("Program shutdown gracefully.\n");
    return 0;
}
