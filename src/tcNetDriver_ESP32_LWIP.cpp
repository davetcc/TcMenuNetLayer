/*
 * Copyright (c) 2018 https://www.thecoderscorner.com (Dave Cherry).
 * This product is licensed under an Apache license, see the LICENSE file in the top-level directory.
 */

#include <Arduino.h>
#include "TcMenuNetLayer.h"

#ifdef TC_NET_USES_ESP32

#include <remote/TransportNetworkDriver.h>
#include <TaskManagerIO.h>
#include <IoLogging.h>
#include <tcUtil.h>
#include "TcNetESP32Extra.h"
#include <esp_wifi.h>
#include <lwip/sockets.h>

#define NET_LOGGING_CHANNEL SER_USER_1
#define LOGGING_IO_OP_DEBUG true
#define MAX_SEND_PER_PACKET 500

namespace tcremote {
    class EspConfiguration {
    public:
        virtual SocketErrCode startConfigurationManual(const uint8_t* ip, const uint8_t* mask)=0;
        virtual SocketErrCode startConfigurationDhcp()=0;
        virtual bool isNetworkUp()=0;
        virtual uint32_t getLocalAddress()=0;
    };

    EspConfiguration *espNetConfig = nullptr;

    class EspWifiConfiguration : public EspConfiguration {
    private:
        const char* mySsid;
        const char* myPwd;
        const uint8_t* myMac;
        bool stationMode;
    public:
        EspWifiConfiguration(const uint8_t* mac, const char* ssid, const char* pwd, bool stationMode) : mySsid(ssid), myPwd(pwd), myMac(mac), stationMode(stationMode) {}

        SocketErrCode startWifi() {
            serlogF2(NET_LOGGING_CHANNEL, "Starting ESP WiFi on SSID", mySsid);
            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

            if(esp_wifi_init(&cfg) != ESP_OK) {
                serlogF(SER_ERROR, "WiFi module not initialised");
                return SOCK_ERR_FAILED;
            }

            serlogF2(NET_LOGGING_CHANNEL, "Initialised WiFi", mySsid);

            esp_netif_inherent_config_t netifCfg;
            if(stationMode) {
                netifCfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
            } else {
                netifCfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
            }

            // Warning: the interface desc is used in tests to capture actual connection details (IP, gw, mask)
            netifCfg.if_desc = "TCStack";
            netifCfg.route_prio = 128;
            memcpy(netifCfg.mac, myMac, sizeof netifCfg.mac);
            auto staNetIf = esp_netif_create_wifi(WIFI_IF_STA, &netifCfg);
            if(staNetIf == nullptr) {
                serlogF(SER_ERROR, "Create WiFi failed");
                return SOCK_ERR_FAILED;
            }
            esp_wifi_set_default_wifi_sta_handlers();

            serlogF(NET_LOGGING_CHANNEL, "Created WiFi stack");

            if(esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
                serlogF(SER_ERROR, "WiFi set store failed");
                return SOCK_ERR_FAILED;
            }

            if(stationMode) {
                if(esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
                    serlogF(SER_ERROR, "Cant set as station");
                    return SOCK_ERR_FAILED;
                }
                wifi_config_t wifiConfig;
                strcpy((char*)wifiConfig.sta.ssid, mySsid);
                strcpy((char*)wifiConfig.sta.password, myPwd);
                if(esp_wifi_set_config(stationMode ? WIFI_IF_STA : WIFI_IF_AP, &wifiConfig) != ESP_OK) {
                    serlogF(SER_ERROR, "WiFi Configuration fail");
                    return SOCK_ERR_FAILED;
                }
                serlogF(NET_LOGGING_CHANNEL, "Station mode set")
            } else {
                serlogF(SER_ERROR, "AP mode not supported yet");
                return SOCK_ERR_FAILED;
            }

            if(esp_wifi_start() != ESP_OK) {
                serlogF(SER_ERROR, "WiFi start failed");
                return SOCK_ERR_FAILED;
            }

            serlogF(NET_LOGGING_CHANNEL, "WiFi started");

            return SOCK_ERR_OK;
        }

        SocketErrCode startConfigurationManual(const uint8_t* ip, const uint8_t* mask) override {
            return SOCK_ERR_UNSUPPORTED;
        }

        SocketErrCode startConfigurationDhcp() override {
            return startWifi();
        }
        bool isNetworkUp() override {
            return false;
        }

        uint32_t getLocalAddress() override {
            return 0;
        }
    };

    SocketErrCode espExtWifiDetails(const char* ssid, const char* pwd, const uint8_t* mac, Esp32NetworkMode mode) {
        if(mode == ESP_ETHERNET) return SOCK_ERR_FAILED;

        espNetConfig = new EspWifiConfiguration(mac, ssid, pwd, mode == ESP_WIFI_STA);
        return espNetConfig != nullptr ? SOCK_ERR_OK : SOCK_ERR_FAILED;
    }

    SocketErrCode startNetLayerDhcp() {
        if(espNetConfig == nullptr) {
            serlogF(SER_ERROR, "ESP cfg is null");
            return SOCK_ERR_FAILED;
        }
        return espNetConfig->startConfigurationDhcp();
    }

    SocketErrCode startNetLayerManual(const uint8_t* ip, const uint8_t* mac, const uint8_t* mask) {
        if(espNetConfig == nullptr) {
            serlogF(SER_ERROR, "ESP cfg is null");
            return SOCK_ERR_FAILED;
        }
        return espNetConfig->startConfigurationManual(ip, mask);
    }

    void copyIpIntoBuffer(uint32_t ip, char* buffer, int bufferSize) {
        ltoaClrBuff(buffer, int(ip & 0xff), 3, NOT_PADDED, bufferSize);
        appendChar(buffer, '.', bufferSize);
        fastltoa(buffer, int(ip >> 8) & 0xff, 3, NOT_PADDED, bufferSize);
        appendChar(buffer, '.', bufferSize);
        fastltoa(buffer, int(ip >> 16) & 0xff, 3, NOT_PADDED, bufferSize);
        appendChar(buffer, '.', bufferSize);
        fastltoa(buffer, int(ip >> 24) & 0xff, 3, NOT_PADDED, bufferSize);
    }

    void copyIpAddress(socket_t theSocket, char* buffer, size_t bufferSize) {

        if(espNetConfig && theSocket == TC_LOCALHOST_SOCKET_ID) {
            copyIpIntoBuffer(espNetConfig->getLocalAddress(), buffer, int(bufferSize));
        } else {
            buffer[0]=0;
        }
    }

    bool isNetworkUp() {
        return espNetConfig && espNetConfig->isNetworkUp();
    }

    class AcceptTaskParams {
    private:
        int listenPort;
        ServerAcceptedCallback callback;
        void* callbackData;
    public:
        TaskHandle_t taskHandle;
        AcceptTaskParams(int port, ServerAcceptedCallback cb, void* cbData) : listenPort(port), callback(cb), callbackData(cbData) {}

        void clientAccepted(socket_t client) { if(callback) callback(client, callbackData); }

        int getListenPort() { return listenPort; }
    };

    void taskAccept(void* taskData) {
        auto params = reinterpret_cast<AcceptTaskParams*>(taskData);

        socket_t acceptSoc = TC_BAD_SOCKET_ID;

        sockaddr_storage listenAddr{};
        auto *addrIp4 = (struct sockaddr_in *)&listenAddr;
        addrIp4->sin_addr.s_addr = htonl(INADDR_ANY);
        addrIp4->sin_family = AF_INET;
        addrIp4->sin_port = htons(params->getListenPort());

        while(1) {
            if(isNetworkUp()) {
                TickType_t xLastWakeTime = xTaskGetTickCount();
                const TickType_t xFrequency = pdMS_TO_TICKS( 5000 );
                if(acceptSoc == TC_BAD_SOCKET_ID) {
                    acceptSoc = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                    if(acceptSoc < 0) {
                        acceptSoc = TC_BAD_SOCKET_ID;
                        serlogF2(SER_ERROR, "No accept socket", params->getListenPort());
                        vTaskDelayUntil(&xLastWakeTime, xFrequency);
                        continue;
                    }
                    int optData = 1;
                    setsockopt(acceptSoc, SOL_SOCKET, SO_REUSEADDR, &optData, sizeof(optData));

                    if(bind(acceptSoc, (sockaddr*)&listenAddr, sizeof(listenAddr)) != 0 || listen(acceptSoc, 4) != 0) {
                        serlogF2(SER_ERROR, "Binding failed ", params->getListenPort())
                        vTaskDelayUntil(&xLastWakeTime, xFrequency);
                        closesocket(acceptSoc);
                        acceptSoc = TC_BAD_SOCKET_ID;
                        continue;
                    }
                    serlogF(NET_LOGGING_CHANNEL, "Accept created");
                } else {
                    socklen_t sockLen = sizeof(listenAddr);
                    int retCode = accept(acceptSoc, (sockaddr*) &listenAddr, sizeof(listenAddr));
                    if(retCode < 0) {
                        serlogF(SER_ERROR, "No connection accepted");
                    } else {

                    }
                }
            } else {
                taskYIELD();
            }
        }
    }

    SocketErrCode initialiseAccept(int port, ServerAcceptedCallback onServerAccepted, void* callbackData) {
        auto atp = new AcceptTaskParams(port, onServerAccepted, callbackData);
        xTaskCreate(taskAccept, "Accept", CONFIG_ARDUINO_LOOP_STACK_SIZE, atp, tskIDLE_PRIORITY, &atp->taskHandle);
        /*for(int i=0; i<MAX_TCP_ACCEPTS; i++) {
            if(tcpSlots[i].getPortNum() == 0) {
                tcpSlots[i].initialise(port, onServerAccepted, callbackData);
                return SOCK_ERR_OK;
            }
        }*/
        return SOCK_ERR_FAILED;
    }

    int rawReadData(socket_t socketNum, void* data, size_t dataLen) {
        return 0;
    }

    bool rawWriteAvailable(socket_t socketNum) {
        return false;
    }


    bool rawReadAvailable(socket_t socketNum) {
        return false;
    }

    SocketErrCode rawWriteData(socket_t socketNum, const void* data, size_t dataLen, MemoryLocationType memType, int timeoutMillis) {
        return SOCK_ERR_FAILED;
    }

    SocketErrCode rawFlushAll(socket_t socketNum) {
        return SOCK_ERR_FAILED;
    }

    void closeSocket(socket_t socketNum) {
        if(socketNum > 0) {
            shutdown(socketNum, 0);
            close(socketNum);
        }
    }
}

#endif //ESP32
