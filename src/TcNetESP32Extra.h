/*
 * Copyright (c) 2018 https://www.thecoderscorner.com (Dave Cherry).
 * This product is licensed under an Apache license, see the LICENSE file in the top-level directory.
 */
/**
 * @file TcNetESP32Extra.h
 *
 * Additional definitions needed to initialise and use the ESP32 WiFi / Ethernet driver.
 */
#ifndef TCLIBRARYDEV_TCNETESP32EXTRA_H
#define TCLIBRARYDEV_TCNETESP32EXTRA_H

#include <Arduino.h>
#ifdef ESP32

#include "remote/TransportNetworkDriver.h"

namespace tcremote {
    /**
     * Defines the mode in which the network capabilities are going to be started
     */
    enum Esp32NetworkMode {
        /** Use the ethernet adapter support - not yet supported */
        ESP_ETHERNET,
        /** Use the WiFi support as an Access point */
        ESP_WIFI_AP,
        /** Use the WiFi support as a station */
        ESP_WIFI_STA
    };

    /**
     * Additional definitions needed to start up the Wi-Fi support, mainly the SSID and Passcode.
     * @param ssid the ssid to use
     * @param pwd the password to use
     * @param mac the adapter mac address
     * @param mode the mode, see the enum for options.
     * @return an error code value to indicate status
     */
    SocketErrCode espExtWifiDetails(const char* ssid, const char* pwd, const uint8_t* mac, Esp32NetworkMode mode);
}

#endif //ESP32
#endif //TCLIBRARYDEV_TCNETESP32EXTRA_H
