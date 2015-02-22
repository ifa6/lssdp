#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>     // close
#include <arpa/inet.h>  // inet_ntop
#include <sys/ioctl.h>  // ioctl, SIOCGIFCONF
#include <net/if.h>     // struct ifconf, struct ifreq
#include "lssdp.h"

/* This is defined on Mac OS X */
#ifndef _SIZEOF_ADDR_IFREQ
#define _SIZEOF_ADDR_IFREQ sizeof
#endif

#define LSSDP_IP_LEN            16
#define LSSDP_MESSAGE_MAX_LEN   2048
#define LSSDP_MULTICAST_ADDR    "239.255.255.250"

/* struct: lssdp_packet */
typedef struct lssdp_packet {
    char            method      [LSSDP_HEADER_FIELD_LEN];   // M-SEARCH, NOTIFY, RESPONSE
    char            st          [LSSDP_HEADER_FIELD_LEN];   // Search Target
    char            usn         [LSSDP_HEADER_FIELD_LEN];   // Unique Service Name
    char            location    [LSSDP_HEADER_FIELD_LEN];   // Location

    /* Additional SSDP Header Fields */
    char            sm_id       [LSSDP_HEADER_FIELD_LEN];
    char            device_type [LSSDP_HEADER_FIELD_LEN];
    unsigned long   update_time;
} lssdp_packet;

// SSDP Method
static const char * LSSDP_MSEARCH  = "M-SEARCH";
static const char * LSSDP_NOTIFY   = "NOTIFY";
static const char * LSSDP_RESPONSE = "RESPONSE";

// SSDP Method Header
static const char * LSSDP_MSEARCH_HEADER  = "M-SEARCH * HTTP/1.1\r\n";
static const char * LSSDP_NOTIFY_HEADER   = "NOTIFY * HTTP/1.1\r\n";
static const char * LSSDP_RESPONSE_HEADER = "HTTP/1.1 200 OK\r\n";

static const char * LSSDP_UDA_v1_1 =
    "OPT:\"http://schemas.upnp.org/upnp/1/0/\"; ns=01\r\n"  /* UDA v1.1 */
    "01-NLS:1\r\n"                                          /* same as BOOTID. UDA v1.1 */
    "BOOTID.UPNP.ORG:1\r\n"                                 /* UDA v1.1 */
    "CONFIGID.UPNP.ORG:1337\r\n";                           /* UDA v1.1 */

#define lssdp_debug(fmt, agrs...) lssdp_log("DEBUG", __LINE__, __func__, fmt, ##agrs)
#define lssdp_warn(fmt, agrs...)  lssdp_log("WARN",  __LINE__, __func__, fmt, ##agrs)
#define lssdp_error(fmt, agrs...) lssdp_log("ERROR", __LINE__, __func__, fmt, ##agrs)

static int send_multicast_data(const char * data, const struct lssdp_interface interface, int ssdp_port);
static int lssdp_packet_parser(const char * data, size_t data_len, lssdp_packet * packet);
static int parse_field_line(const char * data, size_t start, size_t end, lssdp_packet * packet);
static int get_colon_index(const char * string, size_t start, size_t end);
static int lssdp_log(const char * level, int line, const char * func, const char * format, ...);


/** Global Variable **/
static int (* lssdp_log_callback)(const char * file, const char * tag, const char * level, int line, const char * func, const char * message) = NULL;


// 01. lssdp_set_log_callback
int lssdp_set_log_callback(int (* callback)(const char * file, const char * tag, const char * level, int line, const char * func, const char * message)) {
    lssdp_log_callback = callback;
    return 0;
}

// 02. lssdp_get_network_interface
int lssdp_get_network_interface(lssdp_ctx * lssdp) {
    if (lssdp == NULL) {
        lssdp_error("lssdp should not be NULL\n");
        return -1;
    }

    // reset lssdp->interface
    memset(lssdp->interface, 0, sizeof(struct lssdp_interface) * LSSDP_INTERFACE_LIST_SIZE);

    int result = -1;

    /* Reference to this article:
     * http://stackoverflow.com/a/8007079
     */

    // create socket
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        lssdp_error("create socket failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    char buffer[4096] = {0};
    struct ifconf ifc = {
        .ifc_len = sizeof(buffer),
        .ifc_buf = (caddr_t) buffer
    };

    if (ioctl(fd, SIOCGIFCONF, &ifc) < 0) {
        lssdp_error("ioctl failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    size_t index, num = 0;
    struct ifreq * ifr;
    for (index = 0; index < ifc.ifc_len; index += _SIZEOF_ADDR_IFREQ(*ifr)) {
        ifr = (struct ifreq *)(buffer + index);
        if (ifr->ifr_addr.sa_family != AF_INET) {
            // only support IPv4
            continue;
        }

        // get interface ip string
        char ip[LSSDP_IP_LEN] = {0};  // ip = "xxx.xxx.xxx.xxx"
        struct sockaddr_in * addr_in = (struct sockaddr_in *) &ifr->ifr_addr;
        if (inet_ntop(AF_INET, &addr_in->sin_addr, ip, sizeof(ip)) == NULL) {
            lssdp_error("inet_ntop failed, errno = %s (%d)\n", strerror(errno), errno);
            goto end;
        }

        // too many network interface
        if (num >= LSSDP_INTERFACE_LIST_SIZE) {
            lssdp_warn("the number of network interface is over than max size %d\n", LSSDP_INTERFACE_LIST_SIZE);
            lssdp_debug("%2d. %s : %s\n", num, ifr->ifr_name, ip);
        } else {
            // 1. set interface.name
            snprintf(lssdp->interface[num].name, LSSDP_INTERFACE_NAME_LEN - 1, "%s", ifr->ifr_name);

            // 2. set interface.ip = [ xxx, xxx, xxx, xxx ]
            char * token_ptr;
            size_t i;
            for (i = 0; i < 4; i++) {
                lssdp->interface[num].ip[i] = atoi(strtok_r(i == 0 ? ip : NULL, ".", &token_ptr));
            }
        }

        // increase interface number
        num++;
    }

    result = 0;
end:
    if (fd > 0) close(fd);
    return result;
}

// 03. lssdp_create_socket
int lssdp_create_socket(lssdp_ctx * lssdp) {
    if (lssdp == NULL) {
        lssdp_error("lssdp should not be NULL\n");
        return -1;
    }

    if (lssdp->sock >= 0) {
        lssdp_debug("close socket %d\n", lssdp->sock);
        close(lssdp->sock);
        lssdp->sock = -1;
    }

    // create UDP socket
    lssdp->sock = socket(PF_INET, SOCK_DGRAM, 0);
    if (lssdp->sock < 0) {
        lssdp_error("create socket failed, errno = %s (%d)\n", strerror(errno), errno);
        return -1;
    }

    int result = -1;

    // set non-blocking
    int opt = 1;
    if (ioctl(lssdp->sock, FIONBIO, &opt) != 0) {
        lssdp_error("ioctl failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // set reuse address
    if (setsockopt(lssdp->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        lssdp_error("setsockopt SO_REUSEADDR failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // bind socket
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(lssdp->port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(lssdp->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        lssdp_error("bind failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // set IP_ADD_MEMBERSHIP
    struct ip_mreq imr = {
        .imr_multiaddr.s_addr = inet_addr(LSSDP_MULTICAST_ADDR),
        .imr_interface.s_addr = htonl(INADDR_ANY)
    };
    if (setsockopt(lssdp->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imr, sizeof(struct ip_mreq)) != 0) {
        lssdp_error("setsockopt IP_ADD_MEMBERSHIP failed: %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    result = 0;
end:
    if (result == -1) {
        close(lssdp->sock);
        lssdp->sock = -1;
    }
    return result;
}

// 04. lssdp_read_socket
int lssdp_read_socket(lssdp_ctx * lssdp) {
    char buffer[2048] = {};
    struct sockaddr_in address = {};
    socklen_t address_len = sizeof(struct sockaddr_in);

    ssize_t recv_len = recvfrom(lssdp->sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&address, &address_len);
    if (recv_len == -1) {
        lssdp_error("recvfrom failed, errno = %s (%d)\n", strerror(errno), errno);
        return -1;
    }

    // TODO: parse SSDP packet to struct

    if (lssdp->data_callback == NULL) {
        lssdp_warn("data_callback has not been setup\n");
        return 0;
    }

    // invoke data received callback
    lssdp->data_callback(lssdp, buffer, recv_len);
    return 0;
}

// 05. lssdp_send_msearch
int lssdp_send_msearch(lssdp_ctx * lssdp) {
    // 1. update network interface
    lssdp_get_network_interface(lssdp);

    // 2. set M-SEARCH packet
    char msearch[1024] = {};
    snprintf(msearch, sizeof(msearch),
        "%s"
        "HOST:%s:%d\r\n"
        "MAN:\"ssdp:discover\"\r\n"
        "ST:%s\r\n"
        "MX:1\r\n"
        "\r\n",
        LSSDP_MSEARCH_HEADER,
        LSSDP_MULTICAST_ADDR, lssdp->port,  // HOST
        lssdp->header.st                    // ST (Search Target)
    );

    // 3. send M-SEARCH to each interface
    size_t i;
    for (i = 0; i < LSSDP_INTERFACE_LIST_SIZE; i++) {
        struct lssdp_interface * interface = &lssdp->interface[i];
        if (strlen(interface->name) == 0) {
            break;
        }

        send_multicast_data(msearch, *interface, lssdp->port);
    }
    return 0;
}

// 06. lssdp_send_notify
int lssdp_send_notify(lssdp_ctx * lssdp) {
    // update network interface
    lssdp_get_network_interface(lssdp);

    // 1. set location suffix
    char suffix[256] = {0};
    const int port = lssdp->header.location.port;
    if (0 < port && port <= 0xFFFF) {
        sprintf(suffix, ":%d", port);
    }
    const char * uri = lssdp->header.location.uri;
    if (strlen(uri) > 0) {
        sprintf(suffix + strlen(suffix), "/%s", uri);
    }

    // 2. send NOTIFY to each interface
    size_t i;
    for (i = 0; i < LSSDP_INTERFACE_LIST_SIZE; i++) {
        struct lssdp_interface * interface = &lssdp->interface[i];
        if (strlen(interface->name) == 0) {
            break;
        }

        // set location: Host (or interface IP) + suffix
        char location[256] = {};
        const char * host = lssdp->header.location.host;
        if (strlen(host) > 0) {
            sprintf(location, "%s%s", host, suffix);
        } else {
            sprintf(location, "%d.%d.%d.%d%s",
                interface->ip[0],
                interface->ip[1],
                interface->ip[2],
                interface->ip[3],
                suffix
            );
        }

        // set notify packet
        char notify[1024] = {};
        snprintf(notify, sizeof(notify),
            "%s"
            "HOST:%s:%d\r\n"
            "CACHE-CONTROL:max-age=120\r\n"
            "ST:%s\r\n"
            "USN:%s\r\n"
            "LOCATION:%s\r\n"
            "SM_ID:%s\r\n"
            "DEV_TYPE:%s\r\n"
            "%s"
            "NTS:ssdp:alive\r\n"
            "\r\n",
            LSSDP_NOTIFY_HEADER,
            LSSDP_MULTICAST_ADDR, lssdp->port,  // HOST
            lssdp->header.st,                   // ST
            lssdp->header.usn,                  // USN
            location,                           // LOCATION
            lssdp->header.sm_id,                // SM_ID    (addtional field)
            lssdp->header.device_type,          // DEV_TYPE (addtional field)
            LSSDP_UDA_v1_1                      // UDA v1.1
        );

        send_multicast_data(notify, *interface, lssdp->port);
    }
    return 0;
}


/** Internal Function **/

static int send_multicast_data(const char * data, const struct lssdp_interface interface, int ssdp_port) {
    if (data == NULL) {
        lssdp_error("data should not be NULL\n");
        return -1;
    }

    size_t data_len = strlen(data);
    if (data_len == 0) {
        lssdp_error("data length should not be empty\n");
        return -1;
    }

    if (strlen(interface.name) == 0) {
        lssdp_error("interface.name should not be empty\n");
        return -1;
    }

    if (ssdp_port < 0 || ssdp_port > 0xFFFF) {
        lssdp_error("ssdp_port (%d) is invalid\n");
        return -1;
    }

    int result = -1;

    // 1. create UDP socket
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        lssdp_error("create socket failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // 2. get IP address
    char ip[LSSDP_IP_LEN] = {};
    sprintf(ip, "%d.%d.%d.%d", interface.ip[0], interface.ip[1], interface.ip[2], interface.ip[3]);

    // 3. bind socket
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = inet_addr(ip)
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        lssdp_error("bind failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // 4. disable IP_MULTICAST_LOOP
    char opt = 0;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &opt, sizeof(opt)) < 0) {
        lssdp_error("setsockopt IP_MULTICAST_LOOP failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // 5. set destination address
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(ssdp_port)
    };
    if (inet_aton(LSSDP_MULTICAST_ADDR, &dest_addr.sin_addr) == 0) {
        lssdp_error("inet_aton failed, errno = %s (%d)\n", strerror(errno), errno);
        goto end;
    }

    // 6. send data
    if (sendto(fd, data, data_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == -1) {
        lssdp_error("sendto %s (%s) failed, errno = %s (%d)\n", interface.name, ip, strerror(errno), errno);
        goto end;
    }

    result = 0;
end:
    if (fd > 0) close(fd);
    return result;
}

static int lssdp_packet_parser(const char * data, size_t data_len, lssdp_packet * packet) {
    if (data == NULL) {
        lssdp_error("data should not be NULL\n");
        return -1;
    }

    if (data_len != strlen(data)) {
        lssdp_error("data_len (%zu) is not match to the data length (%zu)\n", data_len, strlen(data));
        return -1;
    }

    if (packet == NULL) {
        lssdp_error("packet should not be NULL\n");
        return -1;
    }

    // 1. compare SSDP Method Header: M-SEARCH, NOTIFY, RESPONSE
    size_t i;
    if ((i = strlen(LSSDP_MSEARCH_HEADER)) < data_len && memcmp(data, LSSDP_MSEARCH_HEADER, i) == 0) {
        strcpy(packet->method, LSSDP_MSEARCH);
    } else if ((i = strlen(LSSDP_NOTIFY_HEADER)) < data_len && memcmp(data, LSSDP_NOTIFY_HEADER, i) == 0) {
        strcpy(packet->method, LSSDP_NOTIFY);
    } else if ((i = strlen(LSSDP_RESPONSE_HEADER)) < data_len && memcmp(data, LSSDP_RESPONSE_HEADER, i) == 0) {
        strcpy(packet->method, LSSDP_RESPONSE);
    } else {
        lssdp_warn("received unknown SSDP packet\n");
        lssdp_debug("%s\n", data);
        return -1;
    }

    // 2. parse each field line
    size_t start = i;
    for (i = start; i < data_len; i++) {
        if (data[i] == '\n' && i - 1 > start && data[i - 1] == '\r') {
            parse_field_line(data, start, i - 2, packet);
            start = i + 1;
        }
    }

    return 0;
}

static int parse_field_line(const char * data, size_t start, size_t end, lssdp_packet * packet) {
    // 1. find the colon
    if (data[start] == ':') {
        lssdp_warn("the first character of line should not be colon\n");
        lssdp_debug("%s\n", data);
        return -1;
    }

    int colon = get_colon_index(data, start + 1, end);
    if (colon == -1) {
        lssdp_warn("there is no colon in line\n");
        lssdp_debug("%s\n", data);
        return -1;
    }

    if (colon == end) {
        // value is empty
        return -1;
    }

    // TODO: get field, field_len, value, value_len

    return 0;
}

static int get_colon_index(const char * string, size_t start, size_t end) {
    size_t i;
    for (i = start; i <= end; i++) {
        if (string[i] == ':') {
            return i;
        }
    }
    return -1;
}

static int lssdp_log(const char * level, int line, const char * func, const char * format, ...) {
    if (lssdp_log_callback == NULL) {
        return -1;
    }

    char message[LSSDP_MESSAGE_MAX_LEN] = {0};

    // create message by va_list
    va_list args;
    va_start(args, format);
    vsnprintf(message, LSSDP_MESSAGE_MAX_LEN, format, args);
    va_end(args);

    // invoke log callback function
    lssdp_log_callback(__FILE__, "SSDP", level, line, func, message);
    return 0;
}
