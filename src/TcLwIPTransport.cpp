/*
 * Copyright (c) 2018 https://www.thecoderscorner.com (Dave Cherry).
 * This product is licensed under an Apache license, see the LICENSE file in the top-level directory.
 */

#include "TcLwIPTransport.h"
#include <remote/TransportNetworkDriver.h>

using namespace tcremote;

int TcLwIPTransport::writeChar(char data) {
    char d[1];
    auto ret = rawWriteData(client, d, 1, RAM_NEEDS_COPY) ;
    return ret == SOCK_ERR_OK ? 1 : 0;
}

int TcLwIPTransport::writeStr(const char *data) {
    auto ret = rawWriteData(client, data, strlen(data), RAM_NEEDS_COPY);
    return ret == SOCK_ERR_OK ? 1 : 0;
}

void TcLwIPTransport::flush() {
    rawFlushAll(client);
}

bool TcLwIPTransport::available() {
    return rawWriteAvailable(client);
}

bool TcLwIPTransport::readAvailable() {
    return rawWriteAvailable(client);
}

bool TcLwIPTransport::connected() {
    return consideredOpen;
}

uint8_t TcLwIPTransport::readByte() {
    char sz[1];
    return rawReadData(client, sz, 1);
}

void TcLwIPTransport::close() {
    if(consideredOpen) {
        consideredOpen = false;
        return closeSocket(client);
    }
}



