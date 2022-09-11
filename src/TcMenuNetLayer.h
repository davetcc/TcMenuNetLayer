
#ifndef TCLIBRARYDEV_TCMENUNETLAYER_H
#define TCLIBRARYDEV_TCMENUNETLAYER_H

// You can configure th net layer by putting a file with the following name into your source directory.
#if __has_include("TcMenuNetLayerConfig.h")
#include "TcMenuNetLayerConfig.h"
#endif

// attempt to work out what platform we are on
#if defined(ESP32) && !defined(TC_DONT_USE_INTERNAL_LWIP)
#define TC_NET_USES_ESP32
#elif defined(ARDUINO_ARCH_STM32)
#define TC_NET_USES_STM32
#else
#warning "TcNet not supported on this platform"
#endif

// Which channel should the internal logging go to, I normally choose user 1
#ifndef NET_LOGGING_CHANNEL
#define NET_LOGGING_CHANNEL SER_USER_1
#endif //NET_LOGGING_CHANNEL

// Do you want to log write and read data operations, this is very heavy, usually only for debugging
#ifndef LOGGING_IO_OP_DEBUG
#define LOGGING_IO_OP_DEBUG true
#endif //LOGGING_IO_OP_DEBUG

// The maximum amount to send in a single transmission.
#ifndef MAX_SEND_PER_PACKET
#define MAX_SEND_PER_PACKET 500
#endif //MAX_SEND_PER_PACKET

#include <remote/TransportNetworkDriver.h>

#endif //TCLIBRARYDEV_TCMENUNETLAYER_H
