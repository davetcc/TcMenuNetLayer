/*
 * Copyright (c) 2018 https://www.thecoderscorner.com (Dave Cherry).
 * This product is licensed under an Apache license, see the LICENSE file in the top-level directory.
 */

#include <Arduino.h>
#include "../TcMenuNetLayer.h"

#ifdef TC_NET_USES_ESP32

#include "../TransportNetworkDriver.h"
#include <SimpleCollections.h>
#include <SCCircularBuffer.h>
#include <TaskManagerIO.h>
#include <IoLogging.h>
#include <tcUtil.h>
#include "TcNetESP32Extra.h"
#include <esp_wifi.h>
#include <lwip/sockets.h>

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
        bool startedOk;
    public:
        EspWifiConfiguration(const uint8_t* mac, const char* ssid, const char* pwd, bool stationMode);

        SocketErrCode startWifi();

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

    class AcceptTaskHandler : public BaseEvent {
    public:
        enum AcceptTaskStatus { WAITING, ACCEPTING, CONNECTION_FAILURE, BAD_SOCKET };
    private:
        int listenPort;
        ServerAcceptedCallback callback;
        void* callbackData;
        GenericCircularBuffer<socket_t> acceptQueue;
        AcceptTaskStatus acceptStatus = WAITING;
    public:
        TaskHandle_t taskHandle = nullptr;
        AcceptTaskHandler(int port, ServerAcceptedCallback cb, void* cbData) : listenPort(port), callback(cb), callbackData(cbData), acceptQueue(5) {}

        void clientAccepted(socket_t client);
        void setStatus(AcceptTaskStatus sts) { acceptStatus = sts; }
        int getListenPort() const { return listenPort; }

        void exec() override;
        uint32_t timeOfNextCheck() override;

        void closeSock(socket_t s);

    };
}

#endif //ESP32
