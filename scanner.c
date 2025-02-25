#include <math.h> // Now first include
#include <libwebsockets.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <windows.h>
#include <time.h>

// Function prototypes
int parse_symbols(const char *response_data);
void refresh_symbols();

#define DEBUG_MODE 0
#define MAX_TRADES 300
#define MAX_SYMBOLS 50
#define MOVE 1
#define DEBOUNCE_TIME 3000 // 3 seconds in milliseconds
#define REFRESH_INTERVAL 60000 // 3 minutes in milliseconds
#ifdef __MINGW32__
    double
    fabs(double x);
#endif

typedef struct
{
    double price;
} Trade;

extern Trade trade_data[MAX_SYMBOLS][MAX_TRADES]; // ✅ Allow access to global trade data
extern int trade_index[MAX_SYMBOLS];              // ✅ Allow access to global trade index
extern unsigned long last_alert_time[MAX_SYMBOLS]; // ✅ Allow access to alert timers
unsigned long last_alert_time[MAX_SYMBOLS] = {0}; // ✅ Define the variable
double last_alert_price[MAX_SYMBOLS] = {0}; // ✅ Track last alerted price





#define LOG_DEBUG(fmt, ...)             \
    do                                  \
    {                                   \
        if (DEBUG_MODE)                 \
            printf(fmt, ##__VA_ARGS__); \
    } while (0)



void remove_bom(char *data)
{
    if (!data || strlen(data) < 3)
        return;

    if ((unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
    {
        size_t len = strlen(data);
        memmove(data, data + 3, len - 2); // Shift left by 3 bytes
        printf("[DEBUG] BOM removed from JSON response.\n");
    }
}

// Structure to track subscription progress
typedef struct
{
    int sub_index;
} per_session_data;

char **symbols = NULL;
int num_symbols = 0;

size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    char *data = (char *)userdata;
    size_t total_size = size * nmemb;

    size_t current_length = strlen(data);
    size_t available_space = 32768 - current_length - 1; // -1 for null terminator

    if (total_size > available_space)
    {
        fprintf(stderr, "[ERROR] Response buffer full, truncating data\n");
        total_size = available_space;
    }

    strncat(data, (char *)ptr, total_size);
    return total_size;
}

// Fetch symbols from HTTP endpoint
int fetch_symbols(const char *url, char *response_data, size_t buffer_size)
{
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        fprintf(stderr, "[ERROR] Failed to initialize cURL\n");
        return -1;
    }

    LOG_DEBUG("[DEBUG] Fetching symbols from: %s\n", url);

    memset(response_data, 0, buffer_size); // ✅ Ensure buffer is empty before writing

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_data);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        fprintf(stderr, "[ERROR] cURL request failed: %s\n", curl_easy_strerror(res));
        return -1;
    }

    // ✅ Remove BOM if present
    remove_bom(response_data);

    return 0;
}

void refresh_symbols()
{
    char response[32768] = {0}; // ✅ Ensure buffer is large enough

    if (fetch_symbols("http://localhost:8080/symbols", response, sizeof(response)) == 0)
    {
        // ✅ Free previous symbol list before replacing it
        for (int i = 0; i < num_symbols; i++)
            free(symbols[i]);
        free(symbols);

        if (parse_symbols(response) == 0)
        {
            printf("[INFO] Symbol list refreshed successfully!\n");

            // ✅ CLEAR OLD TRADE DATA TO AVOID BAD ALERTS
            memset(trade_data, 0, sizeof(trade_data));   // Reset prices
            memset(trade_index, 0, sizeof(trade_index)); // Reset trade history index
            memset(last_alert_time, 0, sizeof(last_alert_time)); // Reset debounce timers
        }
        else
        {
            printf("[ERROR] Failed to parse new symbol list.\n");
        }
    }
    else
    {
        printf("[ERROR] Failed to fetch new symbols from server.\n");
    }
}



// Parse symbols from JSON response
int parse_symbols(const char *response_data)
{
    LOG_DEBUG("[DEBUG] Parsing response data...\n");

    struct json_object *parsed_json = json_tokener_parse(response_data);
    if (!parsed_json)
    {
        fprintf(stderr, "[ERROR] Failed to parse JSON response\n");
        return -1;
    }

    struct json_object *symbols_json;
    if (!json_object_object_get_ex(parsed_json, "symbols", &symbols_json))
    {
        fprintf(stderr, "[ERROR] JSON does not contain 'symbols' key\n");
        json_object_put(parsed_json);
        return -1;
    }

    num_symbols = json_object_array_length(symbols_json);
    LOG_DEBUG("[DEBUG] Found %d symbols in response\n", num_symbols);

    num_symbols = num_symbols > MAX_SYMBOLS ? MAX_SYMBOLS : num_symbols;

    symbols = malloc(num_symbols * sizeof(char *));
    if (!symbols)
    {
        fprintf(stderr, "[ERROR] Failed to allocate memory for symbols\n");
        json_object_put(parsed_json);
        return -1;
    }

    for (size_t i = 0; i < num_symbols; i++)
    {
        struct json_object *symbol_json = json_object_array_get_idx(symbols_json, i);
        symbols[i] = strdup(json_object_get_string(symbol_json));
        if (!symbols[i])
        {
            fprintf(stderr, "[ERROR] Failed to allocate memory for symbol %zu\n", i);
            json_object_put(parsed_json);
            return -1;
        }

        LOG_DEBUG("[DEBUG] Parsed symbol: %s\n", symbols[i]);
    }

    json_object_put(parsed_json);
    return 0;
}



Trade trade_data[MAX_SYMBOLS][MAX_TRADES] = {0};
int trade_index[MAX_SYMBOLS] = {0};

void store_trade(int symbol_index, double price)
{
    if (DEBUG_MODE)
    {
        LOG_DEBUG("[DEBUG] Storing %s: %.2f at %d\n",
                  symbols[symbol_index], price, trade_index[symbol_index]);
    }
    trade_data[symbol_index][trade_index[symbol_index]].price = price;
    trade_index[symbol_index] = (trade_index[symbol_index] + 1) % MAX_TRADES;
}

int get_symbol_index(const char *symbol)
{
    for (int i = 0; i < num_symbols; i++)
    {
        if (strcmp(symbols[i], symbol) == 0)
            return i;
    }
    return -1;
}

void check_price_movement(int symbol_index)
{
    if (trade_index[symbol_index] < 2)
        return;

    int old_idx = (trade_index[symbol_index] - 2) % MAX_TRADES;
    int new_idx = (trade_index[symbol_index] - 1) % MAX_TRADES;

    double old_price = trade_data[symbol_index][old_idx].price;
    double new_price = trade_data[symbol_index][new_idx].price;

    if (old_price <= 0)
        return;

    double change = ((new_price - old_price) / old_price) * 100.0;

    // ✅ Ensure only positive percentage changes trigger an alert
    if (change >= MOVE) // Adjust threshold as needed
    {
        unsigned long current_time = (unsigned long)(clock() * 1000 / CLOCKS_PER_SEC);

        // ✅ Check debounce time before alerting AND ensure the price is still rising
        if (current_time - last_alert_time[symbol_index] >= DEBOUNCE_TIME && new_price > last_alert_price[symbol_index])
        {
            printf("ALERT: %s increased %.2f%% | Current Price: %.2f\n",
                   symbols[symbol_index], change, new_price);

            Beep(750, 100);

            // ✅ Update last alert time & price
            last_alert_time[symbol_index] = current_time;
            last_alert_price[symbol_index] = new_price;
        }
    }
}

// WebSocket callback
static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    per_session_data *pss = (per_session_data *)user;

    switch (reason)
    {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf("Connected!\n");
        if (!pss)
            pss = (per_session_data *)calloc(1, sizeof(per_session_data)); // Ensure it's initialized
        pss->sub_index = 0;
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (pss->sub_index < MAX_SYMBOLS && pss->sub_index < num_symbols) // ✅ Enforce limit
        {
            unsigned char buf[LWS_PRE + 256];
            unsigned char *p = &buf[LWS_PRE];

            int n = snprintf((char *)p, 256,
                             "{\"type\":\"subscribe\",\"symbol\":\"%s\"}",
                             symbols[pss->sub_index]);

            lws_write(wsi, p, n, LWS_WRITE_TEXT);
            printf("Subscribed to: %s (%d/%d)\n", symbols[pss->sub_index], pss->sub_index + 1, MAX_SYMBOLS);

            pss->sub_index++;
            if (pss->sub_index < MAX_SYMBOLS && pss->sub_index < num_symbols)
            {
                lws_callback_on_writable(wsi);
            }
        }
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
    {
        LOG_DEBUG("[DEBUG] Received raw message: %.*s\n", (int)len, (char *)in);

        struct json_object *msg = json_tokener_parse((char *)in);
        if (!msg)
        {
            printf("[ERROR] Failed to parse JSON message.\n");
            break;
        }

        // Check for "type" field
        struct json_object *type_obj;
        if (json_object_object_get_ex(msg, "type", &type_obj))
        {
            const char *type_str = json_object_get_string(type_obj);
            if (type_str)
            {
                if (strcmp(type_str, "ping") == 0)
                {
                    LOG_DEBUG("[DEBUG] Ignoring ping message.\n");
                    json_object_put(msg);
                    break;
                }
                else if (strcmp(type_str, "error") == 0)
                {
                    struct json_object *error_obj;
                    if (json_object_object_get_ex(msg, "error", &error_obj))
                    {
                        printf("[ERROR] Server error: %s\n", json_object_get_string(error_obj));
                    }
                    json_object_put(msg);
                    break;
                }
            }
        }

        // Check for trade data
        struct json_object *data_array;
        if (json_object_object_get_ex(msg, "data", &data_array) &&
            json_object_get_type(data_array) == json_type_array)
        {
            for (size_t i = 0; i < json_object_array_length(data_array); i++)
            {
                struct json_object *trade = json_object_array_get_idx(data_array, i);
                struct json_object *s, *p;

                if (json_object_object_get_ex(trade, "s", &s) &&
                    json_object_object_get_ex(trade, "p", &p))
                {
                    const char *symbol = json_object_get_string(s);
                    double price = json_object_get_double(p);

                    LOG_DEBUG("[DEBUG] Symbol: %s, Price: %.2f\n", symbol, price);

                    int idx = get_symbol_index(symbol);
                    if (idx != -1)
                    {
                        store_trade(idx, price);
                        check_price_movement(idx);
                    }
                }
                else
                {
                    printf("[WARNING] Trade data missing expected fields. Raw: %s\n",
                           json_object_to_json_string(trade));
                }
            }
        }
        else
        {
            printf("[WARNING] Received message missing 'data' array. Raw: %s\n",
                   json_object_to_json_string(msg));
        }

        json_object_put(msg);
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        printf("Connection error!\n");
        break;

    case LWS_CALLBACK_CLOSED:
        printf("Disconnected\n");
        break;

    default:
        break;
    }
    return 0;
}

#include <windows.h>  // ✅ Required for threading

DWORD WINAPI refresh_thread(LPVOID lpParam)
{
    while (1)
    {
        Sleep(REFRESH_INTERVAL); // ✅ Sleep for 3 minutes
        refresh_symbols();
    }
    return 0;
}

int main()
{
    char response[32768] = {0}; // ✅ Ensure buffer is large enough
    if (fetch_symbols("http://localhost:8080/symbols", response, sizeof(response)) || parse_symbols(response))
    {
        fprintf(stderr, "Failed to initialize symbols\n");
        return 1;
    }

    // ✅ Start the refresh thread (runs in the background)
    HANDLE threadHandle = CreateThread(NULL, 0, refresh_thread, NULL, 0, NULL);
    if (threadHandle == NULL)
    {
        printf("[ERROR] Failed to create refresh thread\n");
        return 1;
    }
    CloseHandle(threadHandle); // ✅ No need to keep handle open

    while (1)
    {
        printf("[INFO] Starting WebSocket connection...\n");

        struct lws_context_creation_info info = {0};
        struct lws_protocols protocols[] = {
            {"ws-protocol", callback_ws, sizeof(per_session_data), 0},
            {NULL, NULL, 0, 0}};

        info.protocols = protocols;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT | LWS_SERVER_OPTION_VALIDATE_UTF8;

        struct lws_context *context = lws_create_context(&info);
        if (!context)
        {
            printf("[ERROR] Failed to create WebSocket context, retrying in 5s...\n");
            Sleep(5000);
            continue;
        }

        struct lws_client_connect_info conn_info = {0};
        conn_info.context = context;
        conn_info.address = "ws.finnhub.io";
        conn_info.port = 443;
        conn_info.path = "/?token=crhlrm9r01qjv9rl4bhgcrhlrm9r01qjv9rl4bi0";
        conn_info.host = conn_info.address;
        conn_info.origin = conn_info.address;
        conn_info.protocol = "ws-protocol";
        conn_info.ssl_connection = LCCSCF_USE_SSL;

        struct lws *wsi = lws_client_connect_via_info(&conn_info);
        if (!wsi)
        {
            printf("[ERROR] Failed to connect to WebSocket, retrying in 5s...\n");
            lws_context_destroy(context);
            Sleep(5000);
            continue;
        }

        // ✅ Run WebSocket loop until it disconnects
        while (1)
        {
            if (lws_service(context, 10) < 0)
            {
                printf("[ERROR] WebSocket error, reconnecting in 5s...\n");
                break; // ✅ Break out of the loop to reconnect
            }
        }

        // ✅ WebSocket disconnected, clean up and restart
        printf("[WARNING] WebSocket disconnected, restarting...\n");
        lws_context_destroy(context);
        Sleep(5000); // ✅ Wait before reconnecting
    }

    return 0;
}
