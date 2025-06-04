/*
  enet.c - lwIP driver glue code for WizNet breakout boards

  Part of grblHAL

  Copyright (c) 2023-2025 Terje Io

  grblHAL is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  grblHAL is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with grblHAL. If not, see <http://www.gnu.org/licenses/>.
*/

#include "driver.h"

#if ETHERNET_ENABLE && defined(_WIZCHIP_)

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <lwipopts.h>
#include <lwip/netif.h>
#include "lwip/opt.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "netif/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/init.h"

#ifdef MDNS_ENABLE

#if LWIP_VERSION < ((2 << 24)|(2 << 16))
#define MDNS_TTL ,255
#else
#define MDNS_TTL
#endif

#include "lwip/apps/mdns.h"

#endif

#include "wizchip_conf.h"
#include "socket.h"
#include "port/w5x00_lwip.h"
#include "w5x00_ll_driver.h"

#include "grbl/report.h"
#include "grbl/task.h"
#include "grbl/protocol.h"
#include "grbl/nvs_buffer.h"

#include "networking/networking.h"

#define SOCKET_MACRAW 0

extern uint8_t mac[6];

static char IPAddress[IP4ADDR_STRLEN_MAX], if_name[NETIF_NAMESIZE] = "";
static stream_type_t active_stream = StreamType_Null;
static network_services_t services = {0}, allowed_services;
static nvs_address_t nvs_address;
static network_settings_t ethernet, network;
static on_report_options_ptr on_report_options;
static on_stream_changed_ptr on_stream_changed;
static char netservices[NETWORK_SERVICES_LEN] = "";
static network_flags_t network_status = {};

#if MQTT_ENABLE

static bool mqtt_connected = false;
static on_mqtt_client_connected_ptr on_client_connected;

static void mqtt_connection_changed (bool connected)
{
    mqtt_connected = connected;

    if(on_client_connected)
         on_client_connected(connected);
}

#endif

static network_info_t *get_info (const char *interface)
{
    static network_info_t info;

    if(interface == if_name) {

        memcpy(&info.status, &network, sizeof(network_settings_t));

        info.interface = (const char *)if_name;
        info.is_ethernet = true;
        info.link_up = network_status.link_up;
        info.mbps = 100;
        info.status.services = services;
        *info.mac = *info.status.ip = *info.status.gateway = *info.status.mask = '\0';

        struct netif *netif = netif_default; // netif_get_by_index(0);

        if(netif) {

            if(network_status.link_up) {
                strcpy(info.status.ip, IPAddress);
                ip4addr_ntoa_r(netif_ip_gw4(netif), info.status.gateway, IP4ADDR_STRLEN_MAX);
                ip4addr_ntoa_r(netif_ip_netmask4(netif), info.status.mask, IP4ADDR_STRLEN_MAX);
            }

            strcpy(info.mac, networking_mac_to_string(netif->hwaddr));
        }

#if MQTT_ENABLE
        networking_make_mqtt_clientid(info.mac, info.mqtt_client_id);
#endif

        return &info;
    }

    return NULL;
}

static void report_options (bool newopt)
{
    on_report_options(newopt);

    if(newopt) {
        hal.stream.write(",ETH");
#if FTP_ENABLE
        if(services.ftp)
            hal.stream.write(",FTP");
#endif
#if WEBDAV_ENABLE
        if(services.webdav)
            hal.stream.write(",WebDAV");
#endif
#if MDNS_ENABLE
        if(services.mdns)
            hal.stream.write(",mDNS");
#endif
#if SSDP_ENABLE
        if(services.ssdp)
            hal.stream.write(",SSDP");
#endif
    } else {

        network_info_t *network;

        if((network = get_info(if_name))) {

            hal.stream.write("[WIZCHIP:");
            hal.stream.write(_WIZCHIP_ID_);
            hal.stream.write("]" ASCII_EOL);

            hal.stream.write("[MAC:");
            hal.stream.write(network->mac);
            hal.stream.write("]" ASCII_EOL);

            hal.stream.write("[IP:");
            hal.stream.write(network->status.ip);
            hal.stream.write("]" ASCII_EOL);

            if(active_stream == StreamType_Telnet || active_stream == StreamType_WebSocket) {
                hal.stream.write("[NETCON:");
                hal.stream.write(active_stream == StreamType_Telnet ? "Telnet" : "Websocket");
                hal.stream.write("]" ASCII_EOL);
            }

#if MQTT_ENABLE
            if(*network->mqtt_client_id) {
                hal.stream.write("[MQTT CLIENTID:");
                hal.stream.write(network->mqtt_client_id);
                hal.stream.write(mqtt_connected ? "]" ASCII_EOL : " (offline)]" ASCII_EOL);
            }
#endif
        }
    }
}

#if MDNS_ENABLE

static void mdns_device_info (struct mdns_service *service, void *txt_userdata)
{
    char build[20] = "build=";

    strcat(build, uitoa(GRBL_BUILD));
    mdns_resp_add_service_txtitem(service, "model=grblHAL", 13);
    mdns_resp_add_service_txtitem(service, (char *)txt_userdata, strlen((char *)txt_userdata));
    mdns_resp_add_service_txtitem(service, build, strlen(build));
}

static void mdns_service_info (struct mdns_service *service, void *txt_userdata)
{
    if(txt_userdata)
        mdns_resp_add_service_txtitem(service, (char *)txt_userdata, strlen((char *)txt_userdata));
}

#endif

static void status_event_out (void *data)
{
    networking.event(if_name, (network_status_t){ .value = (uint32_t)data });
}

static void status_event_publish (network_flags_t changed)
{
    task_add_immediate(status_event_out, (void *)((network_status_t){ .changed = changed, .flags = network_status }).value);
}

static void netif_status_callback (struct netif *netif)
{
#if IP_V6
    if(!network_status.link_up || netif->ip_addr.u_addr.ip4.addr == 0)
        return;
#else
    if(!network_status.link_up || netif->ip_addr.addr == 0)
        return;
#endif

    ip4addr_ntoa_r(netif_ip_addr4(netif), IPAddress, IP4ADDR_STRLEN_MAX);

#if TELNET_ENABLE
    if(network.services.telnet && !services.telnet)
        services.telnet =  telnetd_init(network.telnet_port);
#endif

#if FTP_ENABLE
    if(network.services.ftp && !services.ftp)
        services.ftp = ftpd_init(network.ftp_port);
#endif

#if HTTP_ENABLE
    if(network.services.http && !services.http) {
        services.http = httpd_init(network.http_port);
  #if WEBDAV_ENABLE
        if(network.services.webdav && !services.webdav)
            services.webdav = webdav_init();
  #endif
  #if SSDP_ENABLE
        if(network.services.ssdp && !services.ssdp)
            services.ssdp = ssdp_init(get_info(if_name));
  #endif
    }
#endif

#if WEBSOCKET_ENABLE
    if(network.services.websocket && !services.websocket)
        services.websocket = websocketd_init(network.websocket_port);
#endif

#if MDNS_ENABLE
    if(*network.hostname && network.services.mdns && !services.mdns) {

        mdns_resp_init();

        if((services.mdns = mdns_resp_add_netif(netif_default, network.hostname MDNS_TTL) == ERR_OK)) {

            mdns_resp_add_service(netif_default, network.hostname, "_device-info", DNSSD_PROTO_TCP, 0 MDNS_TTL, mdns_device_info, "version=" GRBL_VERSION);

            if(services.http)
                mdns_resp_add_service(netif_default, network.hostname, "_http", DNSSD_PROTO_TCP, network.http_port MDNS_TTL, mdns_service_info, "path=/");
            if(services.webdav)
                mdns_resp_add_service(netif_default, network.hostname, "_webdav", DNSSD_PROTO_TCP, network.http_port MDNS_TTL, mdns_service_info, "path=/");
            if(services.websocket)
                mdns_resp_add_service(netif_default, network.hostname, "_websocket", DNSSD_PROTO_TCP, network.websocket_port MDNS_TTL, mdns_service_info, NULL);
            if(services.telnet)
                mdns_resp_add_service(netif_default, network.hostname, "_telnet", DNSSD_PROTO_TCP, network.telnet_port MDNS_TTL, mdns_service_info, NULL);
            if(services.ftp)
                mdns_resp_add_service(netif_default, network.hostname, "_ftp", DNSSD_PROTO_TCP, network.ftp_port MDNS_TTL, mdns_service_info, "path=/");

//            mdns_resp_announce(netif_default);
        }
    }
#endif

#if MQTT_ENABLE
    if(!mqtt_connected)
        mqtt_connect(get_info(if_name), &network.mqtt);
#endif

#if MODBUS_ENABLE & MODBUS_TCP_ENABLED
    modbus_tcp_client_start();
#endif

    if(!network_status.ip_aquired) {
        network_status.ip_aquired = On;
        status_event_publish((network_flags_t){ .ip_aquired = On });
    }
}

static void link_status_callback (struct netif *netif)
{
    static bool dhcp_running = false;

    bool isLinkUp = netif_is_link_up(netif);

    if(isLinkUp != network_status.link_up) {

        network_flags_t changed = { .link_up = On };

        if((network_status.link_up = isLinkUp)) {
            if(network.ip_mode == IpMode_DHCP && !dhcp_running)
                dhcp_running = dhcp_start(netif_default) == ERR_OK;
        } else if(network.ip_mode == IpMode_DHCP && network_status.ip_aquired) {
            changed.ip_aquired = On;
            network_status.ip_aquired = Off;
        }

        status_event_publish(changed);

        if(network_status.link_up && network.ip_mode == IpMode_Static)
            netif_status_callback(netif);
    }
}

static void link_check (void *data)
{
    int8_t link_ok = false;

    ctlwizchip(CW_GET_PHYLINK, &link_ok);

    wiz_PhyConf cf;

    ctlwizchip(CW_GET_PHYCONF, &cf);

    if(link_ok) {
        if(!netif_is_link_up(netif_default)) {
            netif_set_up(netif_default);
            netif_set_link_up(netif_default);
        }
    } else if(netif_is_link_up(netif_default)) {
        // TODO: stop running services when link is down. Do in protocol event handlers?
       // netif_set_down(netif_default);
        netif_set_link_down(netif_default);
    }

    task_add_delayed(link_check, NULL, LINK_CHECK_INTERVAL);
}

static void enet_poll (void *data)
{
    static uint32_t ms = 0;

    sys_check_timeouts();

    if(network_status.link_up) switch(++ms) {
#if TELNET_ENABLE
        case 1:
            if(services.telnet)
                telnetd_poll();
            break;
#endif
#if WEBSOCKET_ENABLE
        case 2:
            if(services.websocket)
                websocketd_poll();
            break;
#endif
        case 3:
            ms = 0;
#if FTP_ENABLE
            if(services.ftp)
                ftpd_poll();
#endif
#if MODBUS_ENABLE & MODBUS_TCP_ENABLED
            modbus_tcp_client_poll();
#endif
            break;
    }
}

static void enet_process (void *data)
{
    static bool lock = false;
    static struct {
        uint16_t len;
        uint8_t data[ETHERNET_MTU + 100];
    } packet = {0};

    if(lock)
        return;

    lock = true;
    sockint_kind irq = 0;

    if((ctlsocket(SOCKET_MACRAW, CS_GET_INTERRUPT, &irq) == SOCK_OK) || packet.len) {

        if(packet.len || (irq & SIK_RECEIVED)) {

            struct pbuf *p = NULL;

            while(true) {

                if(packet.len == 0)
                    packet.len = recv_lwip(SOCKET_MACRAW, packet.data, sizeof(packet.data));

                if(packet.len) {

                    if((p = pbuf_alloc(PBUF_RAW, packet.len, PBUF_POOL))) {

                        pbuf_take(p, packet.data, packet.len);
                        packet.len = 0;

                        LINK_STATS_INC(link.recv);

                        if(netif_default->input(p, netif_default) != ERR_OK)
                            pbuf_free(p);

                    } else
                        break;
                } else
                    break;
            }
        }

        if(packet.len) {
            task_delete(enet_process, NULL);
            task_add_delayed(enet_process, NULL, 1);
        }

        if(irq & SIK_RECEIVED) {
            irq &= SIK_RECEIVED;
            ctlsocket(SOCKET_MACRAW, CS_CLR_INTERRUPT, &irq);
        }
    }

    lock = false;
}

static ISR_CODE void ISR_FUNC(irq_handler)(void)
{
    if(!task_add_immediate(enet_process, NULL))
        task_add_delayed(enet_process, NULL, 0);
}

bool enet_start (void)
{
    static struct netif ethif;

    if(nvs_address != 0) {

        *IPAddress = '\0';

        memcpy(&network, &ethernet, sizeof(network_settings_t));
        if(!networking_ismemnull(network.mac, sizeof(network.mac)))
            memcpy(mac, network.mac, sizeof(network.mac));

        if(network.telnet_port == 0)
            network.telnet_port = NETWORK_TELNET_PORT;
        if(network.websocket_port == 0)
            network.websocket_port = NETWORK_WEBSOCKET_PORT;
        if(network.http_port == 0)
            network.http_port = NETWORK_HTTP_PORT;
        if(network.ftp_port == 0)
            network.ftp_port = NETWORK_FTP_PORT;
#if MQTT_ENABLE
        if(network.mqtt.port == 0)
            network.mqtt.port = NETWORK_MQTT_PORT;
#endif

        if(wizchip_initialize() == WizChipInit_OK) {

            // Set ethernet chip MAC address
            setSHAR(networking_ismemnull(network.mac, 6) ? mac : network.mac);

            ctlwizchip(CW_RESET_PHY, 0);

            lwip_init();

            if(network.ip_mode == IpMode_Static)
                netif_add(&ethif, (ip_addr_t *)&network.ip, (ip_addr_t *)&network.mask, (ip_addr_t *)&network.gateway, NULL, netif_initialize, netif_input);
            else
                netif_add(&ethif, IP4_ADDR_ANY, IP4_ADDR_ANY, IP4_ADDR_ANY, NULL, netif_initialize, netif_input);

            ethif.name[0] = 'e';
            ethif.name[1] = '0';

            netif_set_default(&ethif);
            netif_set_link_callback(netif_default, link_status_callback);
            netif_set_status_callback(netif_default, netif_status_callback);

            if(socket(SOCKET_MACRAW, Sn_MR_MACRAW, network.telnet_port, 0x00) < 0)
                return false;

            netif_index_to_name(1, if_name);
#if LWIP_NETIF_HOSTNAME
            netif_set_hostname(netif_default, network.hostname);
#endif

            network_status.interface_up = On;
            status_event_publish((network_flags_t){ .interface_up = On });

            wizchip_gpio_interrupt_initialize(SOCKET_MACRAW, irq_handler);


#if MDNS_ENABLE || SSDP_ENABLE || LWIP_IGMP
//            if(network.services.mdns || network.services.ssdp)
//                netif_default->flags |= NETIF_FLAG_IGMP;
#endif

            link_check(NULL);
            task_add_systick(enet_poll, NULL);

        } else {
            strcpy(IPAddress, "N/A");
            task_run_on_startup(report_warning, "Failed to start ethernet stack!");
            return false;
        }
    }

    return nvs_address != 0;
}

static inline void set_addr (char *ip, ip4_addr_t *addr)
{
    memcpy(ip, addr, sizeof(ip4_addr_t));
}

static status_code_t ethernet_set_ip (setting_id_t setting, char *value)
{
    ip_addr_t addr;

    if(ip4addr_aton(value, &addr) != 1)
        return Status_InvalidStatement;

    status_code_t status = Status_OK;

    switch(setting) {

        case Setting_IpAddress:
            set_addr(ethernet.ip, &addr);
            break;

        case Setting_Gateway:
            set_addr(ethernet.gateway, &addr);
            break;

        case Setting_NetMask:
            set_addr(ethernet.mask, &addr);
            break;

#if MQTT_ENABLE
        case Setting_MQTTBrokerIpAddress:
            set_addr(ethernet.mqtt.ip, &addr);
            break;
#endif

        default:
            status = Status_Unhandled;
            break;
    }

    return status;
}

static char *ethernet_get_ip (setting_id_t setting)
{
    static char ip[IPADDR_STRLEN_MAX];

    switch(setting) {

        case Setting_IpAddress:
            ip4addr_ntoa_r((const ip_addr_t *)&ethernet.ip, ip, IPADDR_STRLEN_MAX);
            break;

        case Setting_Gateway:
            ip4addr_ntoa_r((const ip_addr_t *)&ethernet.gateway, ip, IPADDR_STRLEN_MAX);
            break;

        case Setting_NetMask:
            ip4addr_ntoa_r((const ip_addr_t *)&ethernet.mask, ip, IPADDR_STRLEN_MAX);
            break;

#if MQTT_ENABLE
        case Setting_MQTTBrokerIpAddress:
            ip4addr_ntoa_r((const ip_addr_t *)&ethernet.mqtt.ip, ip, IPADDR_STRLEN_MAX);
            break;
#endif

        default:
            *ip = '\0';
            break;
    }

    return ip;
}

static status_code_t ethernet_set_services (setting_id_t setting, uint_fast16_t int_value)
{
    ethernet.services.mask = int_value & allowed_services.mask;

    return Status_OK;
}

static uint32_t ethernet_get_services (setting_id_t id)
{
    return (uint32_t)ethernet.services.mask;
}

static status_code_t ethernet_set_mac (setting_id_t setting, char *value)
{
    return networking_string_to_mac(value, ethernet.mac) ? Status_OK : Status_InvalidStatement;
}

static char *ethernet_get_mac (setting_id_t setting)
{
    return networking_mac_to_string(ethernet.mac);
}

static const setting_group_detail_t ethernet_groups [] = {
    { Group_Root, Group_Networking, "Networking" }
};

static const setting_detail_t ethernet_settings[] = {
    { Setting_NetworkServices, Group_Networking, "Network Services", NULL, Format_Bitfield, netservices, NULL, NULL, Setting_NonCoreFn, ethernet_set_services, ethernet_get_services, NULL, { .reboot_required = On } },
    { Setting_Hostname, Group_Networking, "Hostname", NULL, Format_String, "x(64)", NULL, "64", Setting_NonCore, ethernet.hostname, NULL, NULL, { .reboot_required = On } },
    { Setting_IpMode, Group_Networking, "IP Mode", NULL, Format_RadioButtons, "Static,DHCP,AutoIP", NULL, NULL, Setting_NonCore, &ethernet.ip_mode, NULL, NULL, { .reboot_required = On } },
    { Setting_IpAddress, Group_Networking, "IP Address", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, ethernet_set_ip, ethernet_get_ip, NULL, { .reboot_required = On } },
    { Setting_Gateway, Group_Networking, "Gateway", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, ethernet_set_ip, ethernet_get_ip, NULL, { .reboot_required = On } },
    { Setting_NetMask, Group_Networking, "Netmask", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, ethernet_set_ip, ethernet_get_ip, NULL, { .reboot_required = On } },
    { Setting_NetworkMAC, Group_Networking, "MAC Address", NULL , Format_String, "x(17)", "17", "17", Setting_NonCoreFn, ethernet_set_mac, ethernet_get_mac, NULL, { .allow_null = On, .reboot_required = On } },
    { Setting_TelnetPort, Group_Networking, "Telnet port", NULL, Format_Int16, "####0", "1", "65535", Setting_NonCore, &ethernet.telnet_port, NULL, NULL, { .reboot_required = On } },
#if FTP_ENABLE
    { Setting_FtpPort, Group_Networking, "FTP port", NULL, Format_Int16, "####0", "1", "65535", Setting_NonCore, &ethernet.ftp_port, NULL, NULL, { .reboot_required = On } },
#endif
#if HTTP_ENABLE
    { Setting_HttpPort, Group_Networking, "HTTP port", NULL, Format_Int16, "####0", "1", "65535", Setting_NonCore, &ethernet.http_port, NULL, NULL, { .reboot_required = On } },
#endif
    { Setting_WebSocketPort, Group_Networking, "Websocket port", NULL, Format_Int16, "####0", "1", "65535", Setting_NonCore, &ethernet.websocket_port, NULL, NULL, { .reboot_required = On } },
#if MQTT_ENABLE
    { Setting_MQTTBrokerIpAddress, Group_Networking, "MQTT broker IP Address", NULL, Format_IPv4, NULL, NULL, NULL, Setting_NonCoreFn, ethernet_set_ip, ethernet_get_ip, NULL, { .reboot_required = On } },
    { Setting_MQTTBrokerPort, Group_Networking, "MQTT broker port", NULL, Format_Int16, "####0", "1", "65535", Setting_NonCore, &ethernet.mqtt.port, NULL, NULL, { .reboot_required = On } },
    { Setting_MQTTBrokerUserName, Group_Networking, "MQTT broker username", NULL, Format_String, "x(32)", NULL, "32", Setting_NonCore, &ethernet.mqtt.user, NULL, NULL, { .allow_null = On } },
    { Setting_MQTTBrokerPassword, Group_Networking, "MQTT broker password", NULL, Format_Password, "x(32)", NULL, "32", Setting_NonCore, &ethernet.mqtt.password, NULL, NULL, { .allow_null = On } },
#endif
};

#ifndef NO_SETTINGS_DESCRIPTIONS

static const setting_descr_t ethernet_settings_descr[] = {
    { Setting_NetworkServices, "Network services/protocols to enable." },
    { Setting_Hostname, "Network hostname." },
    { Setting_IpMode, "IP Mode." },
    { Setting_IpAddress, "Static IP address." },
    { Setting_Gateway, "Static gateway address." },
    { Setting_NetMask, "Static netmask." },
    { Setting_NetworkMAC, "Optional MAC address. Tip: get from an unused device, e.g an old router." },
    { Setting_TelnetPort, "(Raw) Telnet port number listening for incoming connections." },
#if FTP_ENABLE
    { Setting_FtpPort, "FTP port number listening for incoming connections." },
#endif
#if HTTP_ENABLE
    { Setting_HttpPort, "HTTP port number listening for incoming connections." },
#endif
    { Setting_WebSocketPort, "Websocket port number listening for incoming connections."
                             "\\nNOTE: WebUI requires this to be HTTP port number + 1."
    },
#if MQTT_ENABLE
    { Setting_MQTTBrokerIpAddress, "IP address for remote MQTT broker. Set to 0.0.0.0 to disable connection." },
    { Setting_MQTTBrokerPort, "Remote MQTT broker portnumber." },
    { Setting_MQTTBrokerUserName, "Remote MQTT broker username." },
    { Setting_MQTTBrokerPassword, "Remote MQTT broker password." },
#endif
};

#endif

static void ethernet_settings_save (void)
{
    hal.nvs.memcpy_to_nvs(nvs_address, (uint8_t *)&ethernet, sizeof(network_settings_t), true);
}

void ethernet_settings_restore (void)
{
    memset(&ethernet, 0, sizeof(network_settings_t));

    strcpy(ethernet.hostname, NETWORK_HOSTNAME);

    ip4_addr_t addr;

    ethernet.ip_mode = (ip_mode_t)NETWORK_IPMODE;

    if(ip4addr_aton(NETWORK_IP, &addr) == 1)
        set_addr(ethernet.ip, &addr);

    if(ip4addr_aton(NETWORK_GATEWAY, &addr) == 1)
        set_addr(ethernet.gateway, &addr);

#if NETWORK_IPMODE == 0
    if(ip4addr_aton(NETWORK_MASK, &addr) == 1)
        set_addr(ethernet.mask, &addr);
#else
    if(ip4addr_aton("255.255.255.0", &addr) == 1)
        set_addr(ethernet.mask, &addr);
#endif

    bmac_eth_get(ethernet.mac);

    ethernet.ftp_port = NETWORK_FTP_PORT;
    ethernet.telnet_port = NETWORK_TELNET_PORT;
    ethernet.http_port = NETWORK_HTTP_PORT;
    ethernet.websocket_port = NETWORK_WEBSOCKET_PORT;
    ethernet.services.mask = allowed_services.mask;

#if MQTT_ENABLE

    ethernet.mqtt.port = NETWORK_MQTT_PORT;

  #ifdef MQTT_IP_ADDRESS
    if(ip4addr_aton(MQTT_IP_ADDRESS, &addr) == 1)
        set_addr(ethernet.mqtt.ip, &addr);
  #endif

  #ifdef MQTT_USERNAME
    strcpy(ethernet.mqtt.user, MQTT_USERNAME);
  #endif
  #ifdef MQTT_PASSWORD
    strcpy(ethernet.mqtt.password, MQTT_PASSWORD);
  #endif

#endif

    hal.nvs.memcpy_to_nvs(nvs_address, (uint8_t *)&ethernet, sizeof(network_settings_t), true);
}

static void ethernet_settings_load (void)
{
    if(hal.nvs.memcpy_from_nvs((uint8_t *)&ethernet, nvs_address, sizeof(network_settings_t), true) != NVS_TransferResult_OK)
        ethernet_settings_restore();

    ethernet.services.mask &= allowed_services.mask;
}

static setting_details_t setting_details = {
    .groups = ethernet_groups,
    .n_groups = sizeof(ethernet_groups) / sizeof(setting_group_detail_t),
    .settings = ethernet_settings,
    .n_settings = sizeof(ethernet_settings) / sizeof(setting_detail_t),
#ifndef NO_SETTINGS_DESCRIPTIONS
    .descriptions = ethernet_settings_descr,
    .n_descriptions = sizeof(ethernet_settings_descr) / sizeof(setting_descr_t),
#endif
    .save = ethernet_settings_save,
    .load = ethernet_settings_load,
    .restore = ethernet_settings_restore
};

static void stream_changed (stream_type_t type)
{
    if(type != StreamType_SDCard)
        active_stream = type;

    if(on_stream_changed)
        on_stream_changed(type);
}

bool enet_init (void)
{
    if((nvs_address = nvs_alloc(sizeof(network_settings_t)))) {

        networking_init();

        on_report_options = grbl.on_report_options;
        grbl.on_report_options = report_options;

        on_stream_changed = grbl.on_stream_changed;
        grbl.on_stream_changed = stream_changed;

#if MQTT_ENABLE
        on_client_connected = mqtt_events.on_client_connected;
        mqtt_events.on_client_connected = mqtt_connection_changed;
#endif

        settings_register(&setting_details);

#if MODBUS_ENABLE & MODBUS_TCP_ENABLED
        modbus_tcp_client_init();
#endif

        networking.get_info = get_info;
        allowed_services.mask = networking_get_services_list((char *)netservices).mask;
    }

    return nvs_address != 0;
}

#endif
