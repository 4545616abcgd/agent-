// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#include "volc_http.h"

#include "webclient.h"
#include "tls_certificate.h"
#include "volc_osal.h"
#include "util/volc_list.h"
#include "util/volc_log.h"

char* volc_http_post(const char* uri, const char* post_data, int data_len)
{
    struct webclient_session* session = NULL;
    char* buffer = NULL;
    int resp_status;
    size_t res_len = 0;

    if (!uri || !post_data || data_len < 0) {
        LOGE("Invalid HTTP POST arguments");
        return NULL;
    }

    /* create webclient session and set header response size */
    session = webclient_session_create(2048, GLOBAL_ROOT_CERT, GLOBAL_ROOT_CERT_LEN);
    if (session == NULL) {
        goto err_out_label;
    }

    if (webclient_header_fields_add(session,
                                    "Content-Type: application/json\r\n") < 0 ||
        webclient_header_fields_add(session, "Content-Length: %d\r\n",
                                    data_len) < 0) {
        LOGE("Failed to prepare HTTP request headers");
        goto err_out_label;
    }

    /* send POST request by default header */
    if ((resp_status = webclient_post(session, uri, post_data, data_len)) != 200) {
        LOGE("webclient POST request failed, response(%d) error.\n", resp_status);
    }

    int response_len = webclient_response(session, (void**) &buffer, &res_len);
    if (response_len < 0) {
        LOGE("Failed to read HTTP response");
        HAL_SAFE_FREE(buffer);
        goto err_out_label;
    }
    LOGD("HTTP POST completed status=%d response_bytes=%d", resp_status,
         response_len);
err_out_label:
    if (session) {
        webclient_close(session);
    }

    return buffer;
}
