/*
 * Copyright (c) 2018 https://www.thecoderscorner.com (Dave Cherry).
 * This product is licensed under an Apache license, see the LICENSE file in the top-level directory.
 */
/**
 * @file TcLwIPTransport.h
 *
 * Contains an implementation of the transport that works with the LwIP driver code
 */

#ifndef TCLIBRARYDEV_TCLWIPTRANSPORT_H
#define TCLIBRARYDEV_TCLWIPTRANSPORT_H

#include <RemoteConnector.h>
#include "TransportNetworkDriver.h"

namespace tcremote {


    class TcLwIPTransport : public TagValueTransport {
    private:
        socket_t client;
        bool consideredOpen;

    public:
        TcLwIPTransport() : TagValueTransport(TagValueTransportType::TVAL_UNBUFFERED), client(-1), consideredOpen(false) {};
        ~TcLwIPTransport() override = default;
        void setClient(socket_t client) {
            this->consideredOpen = true;
            this->client = client;
        }

        int writeChar(char data) override ;
        int writeStr(const char* data) override;
        void flush() override;
        bool available() override;
        bool connected() override;
        uint8_t readByte() override;
        bool readAvailable() override;
        void close() override;

    };

}

#endif //TCLIBRARYDEV_TCLWIPTRANSPORT_H
