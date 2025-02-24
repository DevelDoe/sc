#include <math.h> // Now first include
#include <libwebsockets.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <json-c/json.h>

// Add MinGW-specific workaround
#ifdef __MINGW32__
double fabs(double x);
#endif

#define DEBUG_MODE 1
#define MAX_TRADES 300
#define MAX_SYMBOLS 5

// Structure to track subscription progress
typedef struct
{
    int sub_index;
} per_session_data;

char **symbols = NULL;
int num_symbols = 0;

// libcurl write callback
size_t write_callback(void *ptr, size_t size, size_t nmemb, char *data)
{
    strncat(data, ptr, size * nmemb);
    return size * nmemb;
}

// Fetch symbols from HTTP endpoint
int fetch_symbols(const char *url, char *response_data)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_data);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK ? 0 : -1;
}

// Parse symbols from JSON response
int parse_symbols(const char *response_data)
{
    struct json_object *parsed_json = json_tokener_parse(response_data);
    if (!parsed_json)
        return -1;

    struct json_object *symbols_json;
    if (!json_object_object_get_ex(parsed_json, "symbols", &symbols_json))
    {
        json_object_put(parsed_json);
        return -1;
    }

    num_symbols = json_object_array_length(symbols_json);
    num_symbols = num_symbols > MAX_SYMBOLS ? MAX_SYMBOLS : num_symbols;

    symbols = malloc(num_symbols * sizeof(char *));
    for (size_t i = 0; i < num_symbols; i++)
    {
        struct json_object *symbol_json = json_object_array_get_idx(symbols_json, i);
        symbols[i] = strdup(json_object_get_string(symbol_json));
    }

    json_object_put(parsed_json);
    return 0;
}

// Trade data structures
typedef struct
{
    double price;
} Trade;

Trade trade_data[MAX_SYMBOLS][MAX_TRADES] = {0};
int trade_index[MAX_SYMBOLS] = {0};

void store_trade(int symbol_index, double price)
{
    if (DEBUG_MODE)
    {
        printf("[DEBUG] Storing %s: %.2f at %d\n",
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
    if (fabs(change) >= 0.01)
    {
        printf("ALERT: %s moved %.2f%%\n", symbols[symbol_index], change);
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
        pss->sub_index = 0;
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (pss->sub_index < num_symbols)
        {
            unsigned char buf[LWS_PRE + 256];
            unsigned char *p = &buf[LWS_PRE];

            int n = snprintf((char *)p, 256,
                             "{\"type\":\"subscribe\",\"symbol\":\"%s\"}",
                             symbols[pss->sub_index]);

            lws_write(wsi, p, n, LWS_WRITE_TEXT);
            printf("Subscribed to: %s\n", symbols[pss->sub_index]);

            pss->sub_index++;
            if (pss->sub_index < num_symbols)
            {
                lws_callback_on_writable(wsi);
            }
        }
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
    {
        struct json_object *msg = json_tokener_parse((char *)in);
        if (!msg)
            break;

        struct json_object *s, *p;
        if (json_object_object_get_ex(msg, "s", &s) &&
            json_object_object_get_ex(msg, "p", &p))
        {
            const char *symbol = json_object_get_string(s);
            double price = json_object_get_double(p);

            int idx = get_symbol_index(symbol);
            if (idx != -1)
            {
                store_trade(idx, price);
                check_price_movement(idx);
            }
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

int main() {
    char response[4096] = "";
    if (fetch_symbols("http://localhost:8080/symbols", response) || parse_symbols(response)) {
        fprintf(stderr, "Failed to initialize symbols\n");
        return 1;
    }

    // Initialize trade data
    memset(trade_data, 0, sizeof(trade_data));
    memset(trade_index, 0, sizeof(trade_index));

    struct lws_context_creation_info info = {0};
struct lws_protocols protocols[] = {
    {"ws-protocol", callback_ws, sizeof(per_session_data), 0},
    {NULL, NULL, 0, 0}
};

info.protocols = protocols;
info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
               LWS_SERVER_OPTION_VALIDATE_UTF8; 

    struct lws_context *context = lws_create_context(&info);
    if (!context) return 1;

    struct lws_client_connect_info conn_info = {0};
    conn_info.context = context;
    conn_info.address = "ws.finnhub.io";
    conn_info.port = 443;
    conn_info.path = "/?token=crhlrm9r01qjv9rl4bhgcrhlrm9r01qjv9rl4bi0";
    conn_info.host = conn_info.address;
    conn_info.origin = conn_info.address;
    conn_info.protocol = "ws-protocol";
    conn_info.ssl_connection = LCCSCF_USE_SSL;

    if (!lws_client_connect_via_info(&conn_info)) {
        lws_context_destroy(context);
        return 1;
    }

    while (1) {
        lws_service(context, 0);
    }

    lws_context_destroy(context);
    for (int i = 0; i < num_symbols; i++) free(symbols[i]);
    free(symbols);
    return 0;
}