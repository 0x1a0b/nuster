/*
 * nuster http related functions.
 *
 * Copyright (C) Jiang Wenyuan, < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <nuster/http.h>

/*
 * Used by cache, should move to new one
 */
const char *nuster_http_msgs[NUSTER_HTTP_SIZE] = {
    [NST_HTTP_200] =
        "HTTP/1.0 200 OK\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "200 OK\n",

    [NST_HTTP_400] =
        "HTTP/1.0 400 Bad request\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "400 Bad request\n",

    [NST_HTTP_404] =
        "HTTP/1.0 404 Not Found\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "404 Not Found\n",

    [NUSTER_HTTP_405] =
        "HTTP/1.0 405 Method Not Allowed\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "405 Method Not Allowed\n",

    [NUSTER_HTTP_500] =
        "HTTP/1.0 500 Internal Server Error\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "500 Internal Server Error\n",

    [NUSTER_HTTP_507] =
        "HTTP/1.0 507 Insufficient Storage\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "507 Insufficient Storage\n",
};

struct buffer nuster_http_msg_chunks[NUSTER_HTTP_SIZE];

struct nuster_headers nuster_headers = {
    .server            = nst_str_set("Server"),
    .date              = nst_str_set("Date"),
    .content_length    = nst_str_set("Content-Length"),
    .content_type      = nst_str_set("Content-Type"),
    .transfer_encoding = nst_str_set("Transfer-Encoding"),
    .last_modified     = nst_str_set("Last-Modified"),
    .expires           = nst_str_set("Expires"),
    .cache_control     = nst_str_set("Cache-Control"),
    .etag              = nst_str_set("ETag"),
};



int nuster_req_find_param(char *query_beg, char *query_end,
        char *name, char **value, int *value_len) {

    char equal   = '=';
    char and     = '&';
    char *ptr    = query_beg;
    int name_len = strlen(name);

    while(ptr + name_len + 1 < query_end) {

        if(!memcmp(ptr, name, name_len) && *(ptr + name_len) == equal) {

            if(ptr == query_beg || *(ptr - 1) == and) {
                ptr    = ptr + name_len + 1;
                *value = ptr;

                while(ptr < query_end && *ptr != and) {
                    (*value_len)++;
                    ptr++;
                }

                return NST_OK;
            }

        }

        ptr++;
    }

    return NST_ERR;
}

