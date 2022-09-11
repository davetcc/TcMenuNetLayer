/*
 * Copyright (c) 2018 https://www.thecoderscorner.com (Dave Cherry).
 * This product is licensed under an Apache license, see the LICENSE file in the top-level directory.
 */

#include <Arduino.h>
#include "TcMenuNetLayer.h"

#if defined(TC_NET_USES_STM32)

#include <remote/TransportNetworkDriver.h>
#include <TaskManagerIO.h>
#include "utility/stm32_eth.h"
#include "utility/ethernetif.h"
#include "tcUtil.h"
#include "lwip/timeouts.h"
#include "IoLogging.h"
#include <SCCircularBuffer.h>
#include "TcMenuNetLwIP.h"

#define MAX_TCP_ACCEPTS 2
#define MAX_TCP_CLIENTS 3
// The write buffer is to prevent small packets with only a few bytes from being written.
#define WRITE_BUFFER_SIZE 128

// The read buffer takes data straight off the socket, and must be of a reasonable size, usually at least 1024 bytes
// as it takes an immediate copy of the socket data.
#define READ_BUFFER_SIZE 1024

extern struct netif gnetif;

namespace tcremote {
    err_t tcpDataWasReceived(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
    void tcpErrorCallback(void *arg, err_t err);
    err_t tcpDataSentCallback(void *arg, struct tcp_pcb *tpcb, u16_t len);

    const uint8_t* myMacAddress;

    class StmTcpClient {
    private:
        tcp_struct clientStruct;
        uint8_t writeBuffer[WRITE_BUFFER_SIZE];
        uint16_t writeBufferPos;
        tccollection::SCCircularBuffer readBuffer;
        uint16_t timeOutMillis;
        uint16_t lastWriteTick;
        uint8_t clientNumber;
    public:
        StmTcpClient() : clientStruct{}, writeBuffer{}, writeBufferPos(0), readBuffer(READ_BUFFER_SIZE), timeOutMillis(1000), lastWriteTick(0) {}

        void initialise(tcp_pcb* pcb, unsigned int sockNo);
        SocketErrCode flush();
        SocketErrCode doRawTcpWrite(const uint8_t* buffer, size_t len, bool constMem);
        void tick();
        void close();
        int read(uint8_t * buffer, size_t bufferSize);
        void setWriteTimeout(uint16_t timeout) { timeOutMillis =  timeout; }
        SocketErrCode pushToBuffer(uint8_t data);
        err_t dataRx(tcp_pcb* pcb, pbuf* p, err_t err);

        bool isInUse() const { return clientStruct.pcb != nullptr; }

        socket_t getClientNo() const { return clientNumber; }

        bool readAvailable() const {
            return clientStruct.data.available > 0;
        }

        bool writeAvailable() {
            return true;
        }
    };

    SocketErrCode StmTcpClient::flush() {
        if ((clientStruct.state != TCP_ACCEPTED) && (clientStruct.state != TCP_CONNECTED)) {
            return SOCK_ERR_FAILED;
        }

        if(writeBufferPos == 0) return SOCK_ERR_OK; // nothing to do
        auto err = doRawTcpWrite(writeBuffer, writeBufferPos, false);
        writeBufferPos = 0;

        return err;
    }

    void StmTcpClient::tick() {
        if(clientStruct.pcb != nullptr && lastWriteTick > 0) {
            --lastWriteTick;
            if(lastWriteTick == 0 && writeBufferPos > 0) flush();
        }
    }

    void StmTcpClient::close() {
        if(clientStruct.pcb) {
            if(writeBufferPos != 0) {
                serlogF2(SER_WARNING, "Close with buffer full", clientNumber);
            }
            tcp_connection_close(clientStruct.pcb, &clientStruct);
            // clear the read buffer out.
            while(readBuffer.available()) readBuffer.get();
        }
    }

    SocketErrCode StmTcpClient::doRawTcpWrite(const uint8_t *buffer, size_t len, bool constMem) {
        size_t left = len;
        uint32_t then = millis();
        size_t posn = 0;

        do {
            if(!clientStruct.pcb) return SOCK_ERR_FAILED;
            auto rawSendSize = uint16_t(tcp_sndbuf(clientStruct.pcb));
            size_t maxSendSize = min(uint16_t(MAX_SEND_PER_PACKET), rawSendSize);

            if(rawSendSize > MAX_SEND_PER_PACKET) {
                size_t thisTime = left > maxSendSize ? maxSendSize : left;
                unsigned int flags = TCP_WRITE_FLAG_MORE;
                if(constMem) flags |= TCP_WRITE_FLAG_COPY;
                auto err = tcp_write(clientStruct.pcb, &buffer[posn], thisTime,  flags);
                if (err != ERR_OK) {
                    serlogF4(NET_LOGGING_CHANNEL, "Socket write error, len", clientNumber, err, thisTime);
                    return SOCK_ERR_FAILED;
                } else {
#ifdef LOGGING_IO_OP_DEBUG
                    serlogF4(NET_LOGGING_CHANNEL, "Written this time, left ", clientNumber, thisTime, left);
#endif
                }

                left -= thisTime;
                posn += thisTime;

                if (ERR_OK != tcp_output(clientStruct.pcb)) {
                    return SOCK_ERR_FAILED;
                }
                // give other tasks chance to run
                taskManager.yieldForMicros(millisToMicros(20));

            } else {
                // if we are getting ahead of ourselves by too far, or we've run out of space we back off
                serlogF3(NET_LOGGING_CHANNEL, "Socket write queue full ", clientNumber, maxSendSize);
                // give other tasks chance to run
                taskManager.yieldForMicros(millisToMicros(100));

            }

            stm32_eth_scheduler();


            if ((millis() - then) > timeOutMillis) return SOCK_ERR_TIMEOUT;
        } while(left > 0);
        return SOCK_ERR_OK;
    }

    int StmTcpClient::read(uint8_t *buffer, size_t bufferSize) {
        size_t pos = 0;
        while(readBuffer.available() && pos < bufferSize) {
            buffer[pos] = readBuffer.get();
            pos++;
        }
        // Number of bytes read into buffer
        return (int)pos;
    }

    SocketErrCode StmTcpClient::pushToBuffer(uint8_t data) {
        if(writeBufferPos >= WRITE_BUFFER_SIZE) {
            auto ret = flush();
            if(ret != SOCK_ERR_OK) {
                close();
                return ret;
            }
        }

        writeBuffer[writeBufferPos++] = data;
        if(lastWriteTick == 0) lastWriteTick = 3;
        return SOCK_ERR_OK;
    }

    err_t StmTcpClient::dataRx(tcp_pcb *pcb, pbuf *p, err_t err) {
        err_t ret_err;

        /* if we receive an empty tcp frame from server => close connection */
        if (p == nullptr) {
            /* probably a closed socket here, we ignore, the higher level protocols will time this out */
            ret_err = ERR_OK;
        } else if (err != ERR_OK) {
            /* free received pbuf*/
            pbuf_free(p);
            ret_err = err;
        } else if ((clientStruct.state == TCP_CONNECTED) || (clientStruct.state == TCP_ACCEPTED)) {
            /* Acknowledge data reception and store*/
            tcp_recved(pcb, p->tot_len);

            pbuf* buff = p;
            bool goAgain = true;
            while(buff && goAgain) {
                goAgain = buff->tot_len != buff->len;
                for (size_t i = 0; i < buff->len; i++) {
                    auto data = (uint8_t*)buff->payload;
                    readBuffer.put(data[i]);
                }
                buff = buff->next;
            }
            pbuf_free(p);
            ret_err = ERR_OK;
        } else {
            /* data received when connection already closed */
            /* Acknowledge data reception */
            tcp_recved(pcb, p->tot_len);

            /* free pbuf and do nothing */
            pbuf_free(p);
            ret_err = ERR_OK;
        }
        return ret_err;
    }

    void StmTcpClient::initialise(tcp_pcb *pcb, unsigned int sockNo) {
        clientStruct.pcb = pcb;
        clientStruct.state = TCP_ACCEPTED;
        clientStruct.data.p = nullptr;
        clientStruct.data.available = 0;
        writeBufferPos = 0;
        lastWriteTick = 0;
        clientNumber = sockNo;
        serlogF2(NET_LOGGING_CHANNEL, "Client accept to ", sockNo);
        tcp_arg(pcb, this);
        tcp_recv(pcb, tcpDataWasReceived);
        tcp_err(pcb, tcpErrorCallback);
        tcp_sent(pcb, tcpDataSentCallback);
    }

    err_t tcpDataWasReceived(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
        auto* client = reinterpret_cast<StmTcpClient*>(arg);
        return client->dataRx(tpcb, p, err);
    }

    void tcpErrorCallback(void *arg, err_t err) {
        auto* client = reinterpret_cast<StmTcpClient*>(arg);
        if(err != ERR_OK) {
            serlogF3(NET_LOGGING_CHANNEL, "Network error for ", client->getClientNo(), err)
            client->close();
        }
    }

    err_t tcpDataSentCallback(void *arg, struct tcp_pcb *tpcb, u16_t len) {
        return ERR_OK;
    }

    StmTcpClient tcpClients[MAX_TCP_CLIENTS];

    class StmTcpServer : public BaseEvent {
    private:
        uint16_t portNum;
        tcp_struct tcpServer;
        void* userData;
        ServerAcceptedCallback theCallback;
        tccollection::GenericCircularBuffer<tcp_pcb*> newClientQueue;
    public:
        StmTcpServer() : portNum(0), tcpServer{}, userData(nullptr), theCallback(nullptr), newClientQueue(5) {
        }

        uint16_t getPortNum() const { return portNum; }

        bool initialise(uint16_t port, ServerAcceptedCallback cb, void *theData);
        void onNewClient(tcp_pcb* clientPcb);

        void exec() override;

        uint32_t timeOfNextCheck() override;
    };

    err_t tcpConnectionEstablished(void *arg, struct tcp_pcb *newpcb, err_t err) {
        if(ERR_OK != err) {
            serlogF(SER_WARNING, "Accept error");
            return err;
        } else {
            auto tcpServer = reinterpret_cast<StmTcpServer *>(arg);
            tcpServer->onNewClient(newpcb);
            return ERR_OK;
        }
    }

    bool StmTcpServer::initialise(uint16_t port, ServerAcceptedCallback cb, void *theData) {
        taskManager.registerEvent(this);

        tcpServer.pcb = tcp_new();
        tcp_arg(tcpServer.pcb, this);
        tcpServer.state = TCP_NONE;

        if (ERR_OK != tcp_bind(tcpServer.pcb, IP_ADDR_ANY, port)) {
            memp_free(MEMP_TCP_PCB, tcpServer.pcb);
            tcpServer.pcb = nullptr;
            return false;
        }

        tcpServer.pcb = tcp_listen(tcpServer.pcb);
        theCallback = cb;
        userData = theData;
        tcp_accept(tcpServer.pcb, tcpConnectionEstablished);
        portNum = port;
        return true;
    }

    socket_t nextFreeClient() {
        for(int i=0; i<MAX_TCP_CLIENTS; i++) {
            if(!tcpClients[i].isInUse()) return i;
        }
        return TC_BAD_SOCKET_ID;
    }

    void StmTcpServer::onNewClient(tcp_pcb* clientPcb) {
        tcp_setprio(clientPcb, TCP_PRIO_MIN);
        newClientQueue.put(clientPcb);
        markTriggeredAndNotify();
    }

    void StmTcpServer::exec() {
        while(newClientQueue.available()) {
            int client = nextFreeClient();
            if(client == TC_BAD_SOCKET_ID) {
                // we can't accept at the moment so exit the exec method, we'll try again next call.
                serlogF2(NET_LOGGING_CHANNEL, "Accepted client waiting on ", portNum);
                return;
            }

            // we have a client and an available handler, set up the client now.
            auto pcb = (tcp_pcb*)newClientQueue.get();
            tcpClients[client].initialise(pcb, client);
            theCallback(client, userData);
        }
    }

    uint32_t StmTcpServer::timeOfNextCheck() {
        if(newClientQueue.available() && nextFreeClient() != TC_BAD_SOCKET_ID) {
            markTriggeredAndNotify();
        }
        return millisToMicros(250);
    }

    StmTcpServer tcpSlots[MAX_TCP_ACCEPTS];

    SocketErrCode startNetLayerDhcp() {
        return SOCK_ERR_UNSUPPORTED;
    }

    SocketErrCode startNetLayerManual(const uint8_t* ip, const uint8_t* mac, const uint8_t* mask) {
        stm32_eth_init(mac, ip, ip, mask);
        /* If there is a local DHCP informs it of our manual IP configuration to
        prevent IP conflict */
        stm32_DHCP_manual_config();
        myMacAddress = mac;

        taskManager.scheduleFixedRate(100, [] {
            for(auto & tcpClient : tcpClients) {
                tcpClient.tick();
            }
        });

        return SOCK_ERR_OK;
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
        if(theSocket == TC_LOCALHOST_SOCKET_ID) {
            copyIpIntoBuffer(stm32_eth_get_ipaddr(), buffer, int(bufferSize));
        } else {
            buffer[0]=0;
        }
    }

    bool isNetworkUp() {
        return !(!stm32_eth_is_init()) && stm32_eth_link_up() != 0;
    }

    SocketErrCode initialiseAccept(int port, ServerAcceptedCallback onServerAccepted, void* callbackData) {
        for(int i=0; i<MAX_TCP_ACCEPTS; i++) {
            if(tcpSlots[i].getPortNum() == 0) {
                tcpSlots[i].initialise(port, onServerAccepted, callbackData);
                return SOCK_ERR_OK;
            }
        }
        return SOCK_ERR_FAILED;

    }

    int rawReadData(socket_t socketNum, void* data, size_t dataLen) {
        if(socketNum < 0 || socketNum >= MAX_TCP_CLIENTS || !tcpClients[socketNum].isInUse()) return -1;
        return tcpClients[socketNum].read((uint8_t*)data, dataLen);
    }

    bool rawWriteAvailable(socket_t socketNum) {
        if(socketNum < 0 || socketNum >= MAX_TCP_CLIENTS || !tcpClients[socketNum].isInUse()) return false;
        return tcpClients[socketNum].writeAvailable();
    }


    bool rawReadAvailable(socket_t socketNum) {
        if(socketNum < 0 || socketNum >= MAX_TCP_CLIENTS || !tcpClients[socketNum].isInUse()) return false;
        return tcpClients[socketNum].readAvailable();
    }

    SocketErrCode rawWriteData(socket_t socketNum, const void* data, size_t dataLen, MemoryLocationType locationType, int timeoutMillis) {
        if(socketNum < 0 || socketNum >= MAX_TCP_CLIENTS || !tcpClients[socketNum].isInUse()) return SOCK_ERR_FAILED;
        if(locationType == IN_PROGRAM_MEM) return SOCK_ERR_NO_PROGMEM_SUPPORT;
        if(dataLen > 100) {
            // this is a large data set, flush what we've got and send in one go
            if(tcpClients[socketNum].flush() != SOCK_ERR_OK) return SOCK_ERR_FAILED;
            return tcpClients[socketNum].doRawTcpWrite((uint8_t*)data, dataLen, locationType == CONSTANT_NO_COPY);
        }
        else {
            tcpClients[socketNum].setWriteTimeout(timeoutMillis);
            for (size_t i = 0; i < dataLen; i++) {
                auto ret = tcpClients[socketNum].pushToBuffer(((uint8_t *) data)[i]);
                if (ret != SOCK_ERR_OK) return ret;
            }
        }
        return SOCK_ERR_OK;
    }

    SocketErrCode rawFlushAll(socket_t socketNum) {
        if(socketNum < 0 || socketNum >= MAX_TCP_CLIENTS || !tcpClients[socketNum].isInUse()) return SOCK_ERR_FAILED;
        return tcpClients[socketNum].flush();
    }

    void closeSocket(socket_t socketNum) {
        if(socketNum < 0 || socketNum >= MAX_TCP_CLIENTS || !tcpClients[socketNum].isInUse()) return;
        tcpClients[socketNum].close();
    }
}

#endif //STM32 only
