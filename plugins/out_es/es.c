/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <msgpack.h>
#include <fluent-bit/flb_output.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_network.h>
#include <cjson/cjson.h>

#include "es.h"
#include "es_http.h"
#include "es_config.h"
#include "es_bulk.h"

struct flb_output_plugin out_es_plugin;

/*
 * Convert the internal Fluent Bit data representation to the required
 * one by Elasticsearch.
 *
 * 'Sadly' this process involves to convert from Msgpack to JSON.
 */
static char *es_format(void *data, size_t bytes, int *out_size)
{
    int i;
    int ret;
    int n_size;
    int index_len;
    uint32_t psize;
    size_t off = 0;
    time_t atime;
    char *buf;
    char *p;
    char ptr[256];
    msgpack_unpacked result;
    msgpack_object root;
    msgpack_object map;
    char *j_entry;
    char j_index[ES_BULK_HEADER];
    json_t *j_root;
    json_t *j_map;
    struct es_bulk *bulk;

    /* Iterate the original buffer and perform adjustments */
    msgpack_unpacked_init(&result);

    /* Perform some format validation */
    ret = msgpack_unpack_next(&result, data, bytes, &off);
    if (!ret) {
        return NULL;
    }

    /* We 'should' get an array */
    if (result.data.type != MSGPACK_OBJECT_ARRAY) {
        /*
         * If we got a different format, we assume the caller knows what he is
         * doing, we just duplicate the content in a new buffer and cleanup.
         */
        return NULL;
    }

    root = result.data;
    if (root.via.array.size == 0) {
        return NULL;
    }

    /* JSON root object */
    j_root = json_create_object();
    if (!j_root) {
        return NULL;
    }

    /* Create the bulk composer */
    bulk = es_bulk_create();
    if (!bulk) {
        return NULL;
    }

    /* Format the JSON header required by the ES Bulk API */
    index_len = snprintf(j_index,
                         ES_BULK_HEADER,
                         ES_BULK_INDEX_FMT,
                         "bulk", "test");

    off = 0;
    msgpack_unpacked_destroy(&result);
    msgpack_unpacked_init(&result);
    while (msgpack_unpack_next(&result, data, bytes, &off)) {
        if (result.data.type != MSGPACK_OBJECT_ARRAY) {
            continue;
        }

        /* Each array must have two entries: time and record */
        root = result.data;
        if (root.via.array.size != 2) {
            continue;
        }

        /* Create a map entry */
        j_map = json_create_object();

        atime = root.via.array.ptr[0].via.u64;
        map   = root.via.array.ptr[1];

        n_size = map.via.map.size + 1;

        json_add_to_object(j_map, "@timestamp", json_create_number(atime));
        for (i = 0; i < n_size - 1; i++) {
            msgpack_object *k = &map.via.map.ptr[i].key;
            msgpack_object *v = &map.via.map.ptr[i].val;

            if (k->type != MSGPACK_OBJECT_BIN && k->type != MSGPACK_OBJECT_STR) {
                continue;
            }

            /* Store key */
            psize = k->via.bin.size;
            if (psize <= (sizeof(ptr) - 1)) {
                memcpy(ptr, k->via.bin.ptr, psize);
                ptr[psize] = '\0';
                p = ptr;
            }
            else {
                /* Long JSON map keys have a performance penalty */
                p = malloc(psize + 1);
                memcpy(p, k->via.bin.ptr, psize);
                p[psize] = '\0';
            }

            /* Store value */
            if (v->type == MSGPACK_OBJECT_NIL) {
                json_add_to_object(j_map, p, json_create_null());
            }
            else if (v->type == MSGPACK_OBJECT_BOOLEAN) {
                if (v->via.boolean) {
                    json_add_to_object(j_map, p, json_create_true());
                }
                else {
                    json_add_to_object(j_map, p, json_create_false());
                }
            }
            else if (v->type == MSGPACK_OBJECT_POSITIVE_INTEGER) {
                json_add_to_object(j_map, p,
                                   json_create_number(v->via.u64));
            }
            else if (v->type == MSGPACK_OBJECT_NEGATIVE_INTEGER) {
                json_add_to_object(j_map, p,
                                   json_create_number(v->via.i64));
            }
            else if (v->type == MSGPACK_OBJECT_FLOAT) {
                json_add_to_object(j_map, p,
                                   json_create_number(v->via.f64));
            }
            else if (v->type == MSGPACK_OBJECT_STR) {
            }
            else if (v->type == MSGPACK_OBJECT_BIN) {
            }

            if (p != ptr) {
                free(p);
            }

            /*
             * At this point we have our JSON message, but in order to
             * ingest this data into Elasticsearch we need to compose the
             * Bulk API request, sadly it requires to prepend a JSON entry
             * with details about the target 'index' and 'type' for EVERY
             * message.
             */
            j_entry = json_print_unformatted(j_map);
            json_delete(j_map);

            es_bulk_append(bulk, j_index, index_len, j_entry, strlen(j_entry));
        }
    }

    msgpack_unpacked_destroy(&result);

    *out_size = bulk->len;
    buf = bulk->ptr;
    free(bulk);

    return buf;
}

int cb_es_init(struct flb_config *config)
{
    int ret;
    struct flb_out_es_config *ctx = NULL;

    ret = flb_output_set_context("es", ctx, config);
    if (ret == -1) {
        flb_utils_error_c("Could not set configuration for es output plugin");
    }

    out_es_plugin.host = strdup("127.0.0.1");
    out_es_plugin.port = 9200;

    return 0;
}

int cb_es_flush(void *data, size_t bytes, void *out_context,
                struct flb_config *config)
{
    int n;
    int ret;
    int bytes_out;
    char *pack;
    size_t bytes_sent;
    char buf[1024];
    size_t len;
    char *request;
    struct flb_out_es_config *ctx = out_context;

    /* Convert format */
    pack = es_format(data, bytes, &bytes_out);
    if (!pack) {
        return -1;
    }

    request = es_http_request(pack, bytes_out, &len, ctx, config);
    ret = flb_io_write(&out_es_plugin, request, len, &bytes_sent);
    if (ret == -1) {
        perror("write");
    }
    free(request);
    free(pack);

    n = flb_io_read(&out_es_plugin, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        flb_debug("[ES] API server response:\n%s", buf);
    }

    return bytes_sent;
}

/* Plugin reference */
struct flb_output_plugin out_es_plugin = {
    .name           = "es",
    .description    = "Elasticsearch",
    .cb_init        = cb_es_init,
    .cb_pre_run     = NULL,
    .cb_flush       = cb_es_flush,
    .flags          = FLB_OUTPUT_TCP,
};