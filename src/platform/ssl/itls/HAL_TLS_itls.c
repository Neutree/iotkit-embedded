/*
 * Copyright (c) 2014-2018 Alibaba Group. All rights reserved.
 * License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <stdlib.h>
#include <string.h>
#if defined(_PLATFORM_IS_LINUX_)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/types.h>
    #include <netdb.h>
    #include <signal.h>
    #include <unistd.h>
#endif
#include "itls/ssl.h"
#include "itls/net.h"
#include "itls/debug.h"
#include "itls/platform.h"

#include "itls.h"
#include "iot_import.h"

#define CONFIG_ITLS_TIME_TEST

#define SEND_TIMEOUT_SECONDS (10)

typedef struct _TLSDataParams {
    mbedtls_ssl_context ssl;          /**< iTLS control context. */
    mbedtls_net_context fd;           /**< iTLS network context. */
    mbedtls_ssl_config conf;          /**< iTLS configuration context. */
} TLSDataParams_t, *TLSDataParams_pt;

#define SSL_LOG(format, ...) \
    do { \
        HAL_Printf("[inf] %s(%d): "format"\n", __FUNCTION__, __LINE__, ##__VA_ARGS__);\
        fflush(stdout);\
    }while(0);

/**< set debug log level, 0 close*/
#define DEBUG_LEVEL 0

#if defined(CONFIG_ITLS_TIME_TEST)
#include <sys/time.h>

static struct timeval tv1, tv2;
#endif

static unsigned int _avRandom()
{
    return (((unsigned int)rand() << 16) + rand());
}

static int _ssl_random(void *p_rng, unsigned char *output, size_t output_len)
{
    uint32_t rnglen = output_len;
    uint8_t   rngoffset = 0;

    while (rnglen > 0) {
        *(output + rngoffset) = (unsigned char)_avRandom() ;
        rngoffset++;
        rnglen--;
    }
    return 0;
}

static void _ssl_debug(void *ctx, int level, const char *file, int line, const char *str)
{
    ((void) ctx);
    ((void) level);

    HAL_Printf("%s:%04d: %s", file, line, str);
}

#if defined(_PLATFORM_IS_LINUX_)
static int net_prepare(void)
{
#if ( defined(_WIN32) || defined(_WIN32_WCE) ) && !defined(EFIX64) && \
   !defined(EFI32)
    WSADATA wsaData;
    static int wsa_init_done = 0;

    if (wsa_init_done == 0) {
        if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0) {
            return (MBEDTLS_ERR_NET_SOCKET_FAILED);
        }

        wsa_init_done = 1;
    }
#else
#if !defined(EFIX64) && !defined(EFI32)
    signal(SIGPIPE, SIG_IGN);
#endif
#endif
    return (0);
}

static int mbedtls_net_connect_timeout(mbedtls_net_context *ctx, const char *host,
                                       const char *port, int proto, unsigned int timeout)
{
    int ret;
    struct addrinfo hints, *addr_list, *cur;
    struct timeval sendtimeout;

    if ((ret = net_prepare()) != 0) {
        return (ret);
    }

    /* Do name resolution with both IPv6 and IPv4 */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = proto == MBEDTLS_NET_PROTO_UDP ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_protocol = proto == MBEDTLS_NET_PROTO_UDP ? IPPROTO_UDP : IPPROTO_TCP;

    if (getaddrinfo(host, port, &hints, &addr_list) != 0) {
        return (MBEDTLS_ERR_NET_UNKNOWN_HOST);
    }

    /* Try the sockaddrs until a connection succeeds */
    ret = MBEDTLS_ERR_NET_UNKNOWN_HOST;
    for (cur = addr_list; cur != NULL; cur = cur->ai_next) {
        ctx->fd = (int) socket(cur->ai_family, cur->ai_socktype,
                               cur->ai_protocol);
        if (ctx->fd < 0) {
            ret = MBEDTLS_ERR_NET_SOCKET_FAILED;
            continue;
        }

        sendtimeout.tv_sec = timeout;
        sendtimeout.tv_usec = 0;

        if (0 != setsockopt(ctx->fd, SOL_SOCKET, SO_SNDTIMEO, &sendtimeout, sizeof(sendtimeout))) {
            SSL_LOG("setsockopt error");
        }

        SSL_LOG("setsockopt SO_SNDTIMEO timeout: %ld", sendtimeout.tv_sec);

        if (connect(ctx->fd, cur->ai_addr, cur->ai_addrlen) == 0) {
            ret = 0;
            break;
        }

        close(ctx->fd);
        ret = MBEDTLS_ERR_NET_CONNECT_FAILED;
    }

    freeaddrinfo(addr_list);

    return (ret);
}
#endif

/**
 * @brief This function connects to the specific SSL server with TLS.
 * @param[in] n is the the network structure pointer.
 * @param[in] addr is the Server Host name or IP address.
 * @param[in] port is the Server Port.
 * @param[in] product_key is the product name.
 *
 * @retval       0 : success.
 * @retval     < 0 : fail.
 */
static int _TLSConnectNetwork(TLSDataParams_t *pTlsData, const char *addr, const char *port,
                              const char *product_key)
{
    int ret = -1;

    /*
     * 0. Initialize the RNG and the session data
     */
#if defined(MBEDTLS_DEBUG_C)
    mbedtls_debug_set_threshold(DEBUG_LEVEL);
#endif
    mbedtls_ssl_init(&(pTlsData->ssl));
    mbedtls_net_init(&(pTlsData->fd));
    mbedtls_ssl_config_init(&(pTlsData->conf));

    /*
     * 1. Start the connection
     */
    SSL_LOG("Connecting to /%s/%s...", addr, port);
#if defined(_PLATFORM_IS_LINUX_)
    if (0 != (ret = mbedtls_net_connect_timeout(&(pTlsData->fd),
                    addr, port, MBEDTLS_NET_PROTO_TCP, SEND_TIMEOUT_SECONDS))) {
        SSL_LOG(" failed ! net_connect returned -0x%04x", -ret);
        return ret;
    }
#else
    if (0 != (ret = mbedtls_net_connect(&(pTlsData->fd), addr, port, MBEDTLS_NET_PROTO_TCP))) {
        SSL_LOG(" failed ! net_connect returned -0x%04x", -ret);
        return ret;
    }
#endif
    SSL_LOG(" ok");

    /*
     * 2. Setup stuff
     */
    SSL_LOG("  . Setting up the SSL/TLS structure...");
    if ((ret = mbedtls_ssl_config_defaults(&(pTlsData->conf),
                                           MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        SSL_LOG(" failed! mbedtls_ssl_config_defaults returned %d", ret);
        goto _out;
    }

    mbedtls_ssl_conf_max_version(&pTlsData->conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_min_version(&pTlsData->conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);

    SSL_LOG(" ok");

    mbedtls_ssl_conf_rng(&(pTlsData->conf), _ssl_random, NULL);
    mbedtls_ssl_conf_dbg(&(pTlsData->conf), _ssl_debug, NULL);

    /* "OPTIONAL", set extra data for client authentication */
    if ((ret = mbedtls_ssl_conf_auth_extra(&(pTlsData->conf), product_key, strlen(product_key))) != 0) {
        SSL_LOG(" failed! mbedtls_ssl_config_auth_extra returned %d", ret);
        goto _out;
    }

    if ((ret = mbedtls_ssl_setup(&(pTlsData->ssl), &(pTlsData->conf))) != 0) {
        SSL_LOG("failed! mbedtls_ssl_setup returned %d", ret);
        goto _out;
    }

    mbedtls_ssl_set_bio(&(pTlsData->ssl), &(pTlsData->fd), mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

    /*
      * 3. Handshake
      */
    SSL_LOG("Performing the SSL/TLS handshake...");
#if defined(CONFIG_ITLS_TIME_TEST)
    gettimeofday(&tv1, NULL);
#endif

    while ((ret = mbedtls_ssl_handshake(&(pTlsData->ssl))) != 0) {
        if ((ret != MBEDTLS_ERR_SSL_WANT_READ) && (ret != MBEDTLS_ERR_SSL_WANT_WRITE)) {
            SSL_LOG("failed  ! mbedtls_ssl_handshake returned -0x%04x", -ret);
            goto _out;
        }
    }

#if defined(CONFIG_ITLS_TIME_TEST)
    gettimeofday(&tv2, NULL);

    SSL_LOG("=========================== iTLS handshake used time(usec): %d\n",
            (int)((tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec)));
#endif

    SSL_LOG(" ok");

_out:
    if (ret != 0) {
        mbedtls_net_free(&(pTlsData->fd));
        mbedtls_ssl_free(&(pTlsData->ssl));
        mbedtls_ssl_config_free(&(pTlsData->conf));
    }

    return ret;
}

static int _network_ssl_read(TLSDataParams_t *pTlsData, char *buffer, int len, int timeout_ms)
{
    uint32_t        readLen = 0;
    static int      net_status = 0;
    int             ret = -1;

#if defined(CONFIG_ITLS_TIME_TEST)
    gettimeofday(&tv1, NULL);
#endif

    mbedtls_ssl_conf_read_timeout(&(pTlsData->conf), timeout_ms);
    while (readLen < len) {
        ret = mbedtls_ssl_read(&(pTlsData->ssl), (unsigned char *)(buffer + readLen), (len - readLen));
        if (ret > 0) {
            readLen += ret;
            net_status = 0;
        } else if (ret == 0) {
            /* if ret is 0 and net_status is -2, indicate the connection is closed during last call */
            return (net_status == -2) ? net_status : readLen;
        } else {
            if (MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY == ret) {
                SSL_LOG("ssl recv peer close notify");
                net_status = -2; /* connection is closed */
                break;
            } else if ((MBEDTLS_ERR_SSL_TIMEOUT == ret)
                       || (MBEDTLS_ERR_SSL_CONN_EOF == ret)
                       || (MBEDTLS_ERR_SSL_SESSION_TICKET_EXPIRED == ret)
                       || (MBEDTLS_ERR_SSL_NON_FATAL == ret)) {
                /* read already complete */
                /* if call mbedtls_ssl_read again, it will return 0 (means EOF) */

                return readLen;
            } else {
                SSL_LOG("ssl recv error: code = %d", ret);
                net_status = -1;
                return -1; /* Connection error */
            }
        }
    }

#if defined(CONFIG_ITLS_TIME_TEST)
    gettimeofday(&tv2, NULL);

    SSL_LOG("=========================== iTLS receive data(%d bytes) used time(usec): %d\n",
            readLen, (int)((tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec)));
#endif

    return (readLen > 0) ? readLen : net_status;
}

static int _network_ssl_write(TLSDataParams_t *pTlsData, const char *buffer, int len, int timeout_ms)
{
    uint32_t writtenLen = 0;
    int ret = -1;

#if defined(CONFIG_ITLS_TIME_TEST)
    gettimeofday(&tv1, NULL);
#endif

    while (writtenLen < len) {
        ret = mbedtls_ssl_write(&(pTlsData->ssl), (unsigned char *)(buffer + writtenLen), (len - writtenLen));
        if (ret > 0) {
            writtenLen += ret;
            continue;
        } else if (ret == 0) {
            SSL_LOG("ssl write timeout");
            return 0;
        } else {
            SSL_LOG("ssl write error, code = %d", ret);
            return -1;
        }
    }

#if defined(CONFIG_ITLS_TIME_TEST)
    gettimeofday(&tv2, NULL);

    SSL_LOG("=========================== iTLS send data(%d bytes) used time(usec): %d\n",
             writtenLen,  (int)((tv2.tv_sec - tv1.tv_sec) * 1000000 + (tv2.tv_usec - tv1.tv_usec)));
#endif

    return writtenLen;
}

static void _network_ssl_disconnect(TLSDataParams_t *pTlsData)
{
    mbedtls_ssl_close_notify(&(pTlsData->ssl));
    mbedtls_net_free(&(pTlsData->fd));
    mbedtls_ssl_free(&(pTlsData->ssl));
    mbedtls_ssl_config_free(&(pTlsData->conf));
    SSL_LOG("ssl_disconnect");
}

uintptr_t HAL_iTLS_Establish(
                 const char *host,
                 uint16_t port,
                 const char *product_key)
{
    char port_str[6];
    TLSDataParams_pt pTlsData;

    pTlsData = HAL_Malloc(sizeof(TLSDataParams_t));
    if (NULL == pTlsData) {
        return (uintptr_t)NULL;
    }

    memset(pTlsData, 0x0, sizeof(TLSDataParams_t));

    sprintf(port_str, "%u", port);

    if (0 != _TLSConnectNetwork(pTlsData, host, port_str, product_key)) {
        HAL_Free((void *)pTlsData);
        return (uintptr_t)NULL; 
    }

    return (uintptr_t)pTlsData;
}

int32_t HAL_iTLS_Destroy(uintptr_t handle)
{
    if ((uintptr_t)NULL == handle) {
        SSL_LOG("handle is NULL");
        return 0;
    }

    _network_ssl_disconnect((TLSDataParams_t *)handle);
    HAL_Free((void *)handle);

    return 0;
}

int32_t HAL_iTLS_Write(uintptr_t handle, const char *buf, int len, int timeout_ms)
{
    return _network_ssl_write((TLSDataParams_t *)handle, buf, len, timeout_ms);
}

int32_t HAL_iTLS_Read(uintptr_t handle, char *buf, int len, int timeout_ms)
{
    return _network_ssl_read((TLSDataParams_t *)handle, buf, len, timeout_ms);;
}

