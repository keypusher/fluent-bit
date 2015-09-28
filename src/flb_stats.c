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

/*
 * Initial draft of the stats interface, it basically aims to collect:
 *
 * - Number of events per second
 * - Number of bytes per second
 *
 * Each input/output plugin must have that counter in place
 */

#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>

#include <mk_core.h>
#include <fluent-bit/flb_config.h>
#include <fluent-bit/flb_input.h>
#include <fluent-bit/flb_output.h>
#include <fluent-bit/flb_error.h>
#include <fluent-bit/flb_utils.h>

#define SDP(s)       &s->data[s->n_data]
#define SDP_TIME(s)  *SDP(s).time

static int stats_unix_server(char *path)
{
    int n, ret;
    int buf_len;
    unsigned long len;
    char buf[1024];
    char cmd[1024];
    int server_fd;
    int remote_fd;
    size_t address_length;
    struct sockaddr_un address;
    socklen_t socket_size = sizeof(struct sockaddr_in);
    struct mk_config_listener *listener;

    /* Create listening socket */
    server_fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket() failed");
        return -1;
    }
    unlink(path);

    len = strlen(path);
    address.sun_family = AF_UNIX;
    sprintf(address.sun_path, "%s", path);
    address_length = sizeof(address.sun_family) + len + 1;
    if (bind(server_fd, (struct sockaddr *) &address, address_length) != 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 5) != 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

static FLB_INLINE int handle_input_plugin(void *event)
{
    ssize_t bytes;
    struct flb_stats_in_plugin *sp = (struct flb_stats_in_plugin *) event;
    struct flb_stats_datapoint data;

    /* We have a READ notification */
    bytes = read(sp->pipe[0], &data, sizeof(struct flb_stats_datapoint));
    if (bytes <= 0) {
        return 0;
    }

    if (sp->n_data == -1) {
        sp->n_data = 0;
    }
    else {
        if (SDP_TIME(sp) != data.time) {
            if (sp->n_data + 1 == FLB_STATS_SIZE) {
                sp->n_data = 0;
            }
            else {
                sp->n_data++;
            }
        }
    }
    memcpy(SDP(sp), &data, sizeof(struct flb_stats_datapoint));
}

static FLB_INLINE int handle_output_plugin(void *event)
{
    ssize_t bytes;
    struct flb_stats_out_plugin *sp = (struct flb_stats_out_plugin *) event;
    struct flb_stats_datapoint data;
    struct flb_stats_datapoint *p_data;

    /* We have a READ notification */
    bytes = read(sp->pipe[0],
                 &data,
                 sizeof(struct flb_stats_datapoint));
    if (bytes <= 0) {
        sp->n_data--;
        return -1;
    }

    if (sp->n_data == -1) {
        sp->n_data = 0;
    }
    else {
        if (SDP_TIME(sp) != data.time) {
            if (sp->n_data + 1 == FLB_STATS_SIZE) {
                sp->n_data = 0;
            }
            else {
                sp->n_data++;
            }
        }
    }
    memcpy(SDP(sp), &data, sizeof(struct flb_stats_datapoint));
}

static FLB_INLINE stats_userver_accept(struct flb_stats *stats)
{
    int remote_fd;
    struct sockaddr sock_addr;
    socklen_t socket_size = sizeof(struct sockaddr);

    remote_fd = accept(stats->userver->fd, &sock_addr, &socket_size);
    return remote_fd;
}

static int stats_userver_add(int fd, struct flb_stats *stats)
{
    int ret;
    struct flb_stats_userver_c *client;
    struct flb_stats_userver *userver = stats->userver;

    /* Allocate connection node */
    client = malloc(sizeof(struct flb_stats_userver_c));
    if (!client) {
        return -1;
    }
    client->fd = fd;
    mk_list_add(&client->_head, &userver->clients);

    /*
     * Register the events into the event loop, we only want to know
     * when it disconnects to release resources.
     */
    ret = mk_event_add(stats->evl,
                       client->fd,
                       FLB_STATS_USERVER_C,
                       0,
                       client);
    if (ret == -1) {
        close(client->fd);
        mk_list_del(&client->_head);
        free(client);
        return -1;
    }

    return 0;
}

static void stats_userver_remove(struct flb_stats_userver_c *uc,
                                 struct flb_stats *stats)
{
    int ret;
    struct flb_stats_userver *userver = stats->userver;

    /* Unregister and release resources */
    mk_event_del(stats->evl, &uc->event);
    close(uc->fd);
    mk_list_del(&uc->_head);
    free(uc);
}

/* Create and register unix socket server */
static int flb_stats_userver(struct flb_stats *stats)
{
    int fd;
    int ret;
    struct mk_event *event;
    struct flb_stats_userver *userver;

    /* Create a TCP server based on unix sockets */
    userver = malloc(sizeof(struct flb_stats_userver));
    if (!userver) {
        flb_error("[stats_usrv] no mem!");
        return -1;
    }

    fd = stats_unix_server(FLB_STATS_USERVER_PATH);
    if (fd == -1) {
        flb_error("[stats_usrv] could not create unix server");
        return -1;
    }

    mk_list_init(&userver->clients);
    userver->fd = fd;
    event = &userver->event;
    event->fd     = fd;
    event->mask   = MK_EVENT_EMPTY;
    event->status = MK_EVENT_NONE;
    ret = mk_event_add(stats->evl,
                       userver->fd,
                       FLB_STATS_USERVER,
                       MK_EVENT_READ,
                       userver);
    if (ret == -1) {
        flb_error("[stats_usrv] could not registrate userver fd");
        return -1;
    }

    stats->userver = userver;
    return 0;
}

static void stats_worker(void *data)
{
    int fd;
    int ret;
    struct mk_event *event;
    struct flb_stats *stats = (struct flb_stats *) data;

    /* Initialize the unix socket server */
    flb_stats_userver(stats);

    while (1) {
        mk_event_wait(stats->evl);
        mk_event_foreach(event, stats->evl) {
            if (event->type == FLB_STATS_INPUT_PLUGIN) {
                handle_input_plugin(event);
            }
            else if (event->type == FLB_STATS_OUTPUT_PLUGIN) {
                handle_output_plugin(event);
            }
            else if (event->type == FLB_STATS_USERVER) {
                /* userver connection arrived */
                fd = stats_userver_accept(stats);
                ret = stats_userver_add(fd, stats);
            }
            else if (event->type == FLB_STATS_USERVER_C) {
                /* userver client disconnected */
                stats_userver_remove((struct flb_stats_userver_c *) event,
                                     stats);
            }
        }
    }
}

static int register_input_plugin(struct flb_input_plugin *plugin,
                                 struct flb_stats *stats)
{
    int ret;
    struct flb_stats_in_plugin *sp;

    /* Allocate stats object for this stats entry */
    sp = malloc(sizeof(struct flb_stats_in_plugin));
    if (!sp) {
        return -1;
    }

    /* Create the communication channel (plugin ---> stats worker) */
    ret = pipe(sp->pipe);
    if (ret == -1) {
        flb_error("[stats reg] could not create pipe");
        free(sp);
        return -1;
    }

    sp->event.fd     = sp->pipe[0];
    sp->event.mask   = MK_EVENT_EMPTY;
    sp->event.status = MK_EVENT_NONE;
    ret = mk_event_add(stats->evl,
                       sp->pipe[0],
                       FLB_STATS_INPUT_PLUGIN,
                       MK_EVENT_READ,
                       sp);
    if (ret == -1) {
        close(sp->pipe[0]);
        close(sp->pipe[1]);
        free(sp);
        return -1;
    }
    plugin->stats_fd = sp->pipe[1];
    sp->plugin = plugin;
    sp->n_data = -1;

    /* Register the entry into the stats input plugins list */
    mk_list_add(&sp->_head, &stats->in_plugins);

    flb_debug("[stats] register in plugin: %s",
              plugin->name);
    return 0;
}

static int register_output_plugin(struct flb_output_plugin *plugin,
                                  struct flb_stats *stats)
{
    int ret;
    struct flb_stats_out_plugin *sp;

    /* Allocate stats object for this stats entry */
    sp = malloc(sizeof(struct flb_stats_out_plugin));
    if (!sp) {
        return -1;
    }

    /* Create the communication channel (plugin ---> stats worker) */
    ret = pipe(sp->pipe);
    if (ret == -1) {
        flb_error("[stats reg] could not create pipe");
        free(sp);
        return -1;
    }

    sp->event.fd     = sp->pipe[0];
    sp->event.mask   = MK_EVENT_EMPTY;
    sp->event.status = MK_EVENT_NONE;
    ret = mk_event_add(stats->evl,
                       sp->pipe[0],
                       FLB_STATS_OUTPUT_PLUGIN,
                       MK_EVENT_READ,
                       sp);
    if (ret == -1) {
        close(sp->pipe[0]);
        close(sp->pipe[1]);
        free(sp);
        return -1;
    }

    plugin->stats_fd = sp->pipe[1];
    sp->plugin = plugin;
    sp->n_data = -1;

    /* Register the entry into the stats input plugins list */
    mk_list_add(&sp->_head, &stats->out_plugins);

    flb_debug("[stats] register out plugin: %s",
              plugin->name);
    return 0;
}

/*
 * For each component inside Fluent Bit, create a statistic
 * entry into our stats handler.
 */
static int flb_stats_components_init(struct flb_stats *stats)
{
    struct mk_list *head;
    struct flb_input_plugin *in;
    struct flb_output_plugin *out;

    /* Initialize list headers */
    mk_list_init(&stats->in_plugins);
    mk_list_init(&stats->out_plugins);

    /* Register input plugins */
    mk_list_foreach(head, &stats->config->inputs) {
        in = mk_list_entry(head, struct flb_input_plugin, _head);
        if (in->active == FLB_TRUE) {
            register_input_plugin(in, stats);
        }
    }

    /* Register output plugins */
    mk_list_foreach(head, &stats->config->outputs) {
        out = mk_list_entry(head, struct flb_output_plugin, _head);
        if (out->active == FLB_TRUE) {
            register_output_plugin(out, stats);
        }
    }
}

/*
 * Initialize the worker thread and statistics plugins across the
 * core and input/output plugins.
 */
int flb_stats_init(struct flb_config *config)
{
    struct flb_stats *stats;

    stats = malloc(sizeof(struct flb_stats));
    if (!stats) {
        flb_error("[stats] could not initialize");
        return -1;
    }
    stats->config = config;

    /* Create the event loop */
    stats->evl = mk_event_loop_create(64);
    if (!stats->evl) {
        flb_error("[stats] could not initialize event loop");
        free(stats);
        return -1;
    }

    /* Register components into the stats interface */
    flb_stats_components_init(stats);

    /* Spawn a worker thread*/
    stats->worker_tid = mk_utils_worker_spawn(stats_worker, stats);

    return 0;
}

/* Collect statistics from different components */
int flb_stats_collect(struct flb_config *config)
{
    time_t t;
    struct mk_list *head;
    struct flb_output_plugin *out;
    struct flb_stats *stats;

    t = time(NULL);

    /* Active output plugins */
    mk_list_foreach(head, &config->outputs) {
        out = mk_list_entry(head, struct flb_output_plugin, _head);
        if (out->active == FLB_FALSE) {
            continue;
        }
    }
}

int flb_stats_register(struct mk_event_loop *evl, struct flb_config *config)
{
    int fd;
    struct mk_event *event;

    event = malloc(sizeof(struct mk_event));
    event->mask = MK_EVENT_EMPTY;
    event->status = MK_EVENT_NONE;

    /* Create a timeout caller every one second */
    fd = mk_event_timeout_create(evl, 1, event);
    if (fd == -1) {
        flb_utils_error(FLB_ERR_CFG_FLUSH_CREATE);
        return -1;
    }

    config->stats_fd = fd;
    return fd;
}