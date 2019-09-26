/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>

#include "event.h"

#include "app_manager.h"
#include "bh_memory.h"
#include "coap_ext.h"

typedef struct _subscribe {
    struct _subscribe * next;
    uint32 subscriber_id;
} subscribe_t;

typedef struct _event {
    struct _event *next;
    int subscriber_size;
    subscribe_t * subscribers;
    char url[1]; /* event url */
} event_reg_t;

event_reg_t *g_events = NULL;

static bool find_subscriber(event_reg_t * reg, uint32 id, bool remove_found)
{
    subscribe_t* c = reg->subscribers;
    subscribe_t * prev = NULL;
    while (c) {
        subscribe_t * next = c->next;
        if (c->subscriber_id == id) {
            if (remove_found) {
                if (prev)
                    prev->next = next;
                else
                    reg->subscribers = next;

                bh_free(c);
            }

            return true;
        } else {
            prev = c;
            c = next;
        }
    }

    return false;
}

static bool check_url(const char *url)
{
    if (*url == 0)
        return false;

    return true;
}

bool am_register_event(const char *url, uint32_t reg_client)
{
    event_reg_t *current = g_events;   
    int offset, is_arena_event;

    app_manager_printf("am_register_event adding url:(%s)\n", url);

    if ((offset = check_url_start(url, strlen(url), "/arena/")) > 0) {
        url+=offset;
        is_arena_event = true;
    }

    if (!check_url(url)) {
        app_manager_printf("am_register_event: invaild url:(%s)\n", url);
        return false;
    }
    while (current) {
        if (strcmp(url, current->url) == 0)
            break;
        current = current->next;
    }

    if (current == NULL) {
        if (NULL
                == (current = (event_reg_t *) bh_malloc(
                        offsetof(event_reg_t, url) + strlen(url) + 1))) {
            app_manager_printf("am_register_event: malloc fail\n");
            return false;
        }

        memset(current, 0, sizeof(event_reg_t));
        bh_strcpy_s(current->url, strlen(url) + 1, url);
        current->next = g_events;
        g_events = current;
    }

    if (find_subscriber(current, reg_client, false)) {
        return true;
    } else {
        subscribe_t * s = (subscribe_t*) bh_malloc(sizeof(subscribe_t));
        if (s == NULL)
            return false;

        memset(s, 0, sizeof(subscribe_t));
        s->subscriber_id = reg_client;
        s->next = current->subscribers;
        current->subscribers = s;

        if (is_arena_event == true) {
            // send subscribe event to host
            request_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.url = (char *)url;
            msg.action = COAP_EVENT_SUB; // subscribe event
            msg.payload = (char*) NULL;
            msg.sender = reg_client;
            send_request_to_host(&msg);
        }

        app_manager_printf("client: %d registered event (%s)\n", reg_client,
                url);
    }

    return true;
}

// @url: NULL means the client wants to unregister all its subscribed items
bool am_unregister_event(const char *url, uint32_t reg_client)
{
    event_reg_t *current = g_events, *pre = NULL;

    while (current != NULL) {
        if (url == NULL || strcmp(current->url, url) == 0) {
            event_reg_t * next = current->next;
            if (find_subscriber(current, reg_client, true)) {
                app_manager_printf("client: %d deregistered event (%s)\n",
                        reg_client, current->url);
            }

            // remove the registration if no client subscribe it
            if (current->subscribers == NULL) {
                app_manager_printf("unregister for event deleted url:(%s)\n",
                        current->url);
                if (pre)
                    pre->next = next;
                else
                    g_events = next;
                bh_free(current);
                current = next;
                
                // send unsubscribe notification to host
                request_t msg;
                memset(&msg, 0, sizeof(msg));
                msg.url = (char *)url;
                msg.action = COAP_EVENT_UNSUB; // unsubscribe
                msg.payload = (char*) NULL;
                send_request_to_host(&msg);              
                continue;
            }
        }
        pre = current;
        current = current->next;
    }

    return true;
}

bool event_handle_event_request(request_t *request, const char *event_url,
        uint32_t reg_client)
{
    if (request->action == COAP_PUT) { /* register */
        return am_register_event(event_url, reg_client); 
    } else if (request->action == COAP_DELETE) { /* unregister */
        return am_unregister_event(event_url, reg_client);
    } else if (request->action == COAP_EVENT_PUB) { /* publish from host */
        request->url = (char*)event_url;
        am_publish_event(request, true); // send publish event to modules
/* 
        // send response to host
        response_t response[1] = { 0 };
        make_response_for_request(request, response);
            set_response(response, CONTENT_2_05,
                FMT_ATTR_CONTAINER, (char*) NULL, 0);
        send_response_to_host(response);
*/
        return true;
    } else {
        /* invalid request */
        return false;
    }
    return false;
}

void am_publish_event(request_t *event, bool from_host)
{
    bh_assert(event->action == COAP_EVENT || event->action == COAP_EVENT_PUB);

    printf("publish event: '%s'\n", event->url);

    if (from_host == false && event->action == COAP_EVENT_PUB) { // COAP_EVENT_PUB comes from arena_publish_event()
        send_request_to_host(event); // send arena publish events to host
        return; // do not deliver arena publish events to modules (they will come back through mqtt)
    } 

    event_reg_t *current = g_events;
    while (current) {
        printf ("=? %s\n", current->url);
        if (0 == strcmp(event->url, current->url)) {
            printf("found subs\n");
            subscribe_t* c = current->subscribers;
            while (c) {
                if (c->subscriber_id != ID_HOST) {
                    module_request_handler(event, (void *)c->subscriber_id);
                }
                c = c->next;
            }

            return;
        }

        current = current->next;
    }
}

bool event_is_registered(const char *event_url)
{
    event_reg_t *current = g_events;

    while (current != NULL) {
        if (strcmp(current->url, event_url) == 0) {
            return true;
        }
        current = current->next;
    }

    return false;
}
