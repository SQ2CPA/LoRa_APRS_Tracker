#include <NimBLEDevice.h>
#include "configuration.h"
#include "ax25_utils.h"
#include "lora_utils.h"
#include "ble_utils.h"
#include "display.h"
#include "logger.h"
#include "kiss_protocol.h"


// APPLE - APRS.fi app
#define SERVICE_UUID_0            "00000001-ba2a-46c9-ae49-01b0961f68bb"
#define CHARACTERISTIC_UUID_TX_0  "00000003-ba2a-46c9-ae49-01b0961f68bb"
#define CHARACTERISTIC_UUID_RX_0  "00000002-ba2a-46c9-ae49-01b0961f68bb"

// ANDROID - BLE Terminal app (Serial Bluetooth Terminal from Playstore)
#define SERVICE_UUID_2            "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX_2  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX_2  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer *pServer;
BLECharacteristic *pCharacteristicTx;
BLECharacteristic *pCharacteristicRx;

extern Configuration    Config;
extern Beacon           *currentBeacon;
extern logging::Logger  logger;
extern bool             sendBleToLoRa;
extern bool             bluetoothConnected;
extern String           BLEToLoRaPacket;
extern Beacon           *currentBeacon;


class MyServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) {
        bluetoothConnected = true;
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "BLE", "%s", "BLE Client Connected");
        delay(100);
    }

    void onDisconnect(NimBLEServer* pServer) {
        bluetoothConnected = false;
        logger.log(logging::LoggerLevel::LOGGER_LEVEL_INFO, "BLE", "%s", "BLE client Disconnected, Started Advertising");
        delay(100);
        pServer->startAdvertising();
    }
};

String kissSerialBuffer = "";

class MyCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic) {
        if (Config.bluetooth.type == 2) { // TNC2
            std::string receivedData = pCharacteristic->getValue();
            String receivedString = "";
            for (int i = 0; i < receivedData.length(); i++) {
                receivedString += receivedData[i];
            }
            
            BLEToLoRaPacket = receivedString;
            sendBleToLoRa = true;
        } else if (Config.bluetooth.type == 0) { // AX25 KISS
            std::string receivedData = pCharacteristic->getValue();

            delay(100);

            for (int i = 0; i < receivedData.length(); i++) {
                char character = receivedData[i];

                if (kissSerialBuffer.length() == 0 && character != (char)FEND) {
                    continue;
                }

                kissSerialBuffer += receivedData[i];

                if (character == (char)FEND && kissSerialBuffer.length() > 3) {
                    bool isDataFrame = false;

                    BLEToLoRaPacket = AX25_Utils::decodeKISS(kissSerialBuffer, isDataFrame);

                    if (isDataFrame) {
                        sendBleToLoRa = true;
                        kissSerialBuffer = "";
                    }
                }
            }
        }
    }
};


namespace BLE_Utils {

    void stop() {
        BLEDevice::deinit();
    }
  
    void setup() {
        String id = currentBeacon->callsign;
        String BLEid = id.substring(0, id.indexOf("-")) + "-BLE";
        BLEDevice::init(BLEid.c_str());
        pServer = BLEDevice::createServer();
        pServer->setCallbacks(new MyServerCallbacks());

        BLEService *pService = nullptr;

        if (Config.bluetooth.type == 0) {
            pService = pServer->createService(SERVICE_UUID_0);
            pCharacteristicTx = pService->createCharacteristic(CHARACTERISTIC_UUID_TX_0, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
            pCharacteristicRx = pService->createCharacteristic(CHARACTERISTIC_UUID_RX_0, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
        } else if (Config.bluetooth.type == 2) {
            pService = pServer->createService(SERVICE_UUID_2);
            pCharacteristicTx = pService->createCharacteristic(CHARACTERISTIC_UUID_TX_2, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
            pCharacteristicRx = pService->createCharacteristic(CHARACTERISTIC_UUID_RX_2, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
        }

        if (pService != nullptr) {
            pCharacteristicRx->setCallbacks(new MyCallbacks());
            pService->start();

            BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();

            if (Config.bluetooth.type == 0) {
                pAdvertising->addServiceUUID(SERVICE_UUID_0);
            } else if (Config.bluetooth.type == 2) {
                pAdvertising->addServiceUUID(SERVICE_UUID_2);
            }
            pServer->getAdvertising()->setScanResponse(true);
            pServer->getAdvertising()->setMinPreferred(0x06);
            pServer->getAdvertising()->setMaxPreferred(0x0C);
            pAdvertising->start();
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE", "%s", "Waiting for BLE central to connect...");
        } else {
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_ERROR, "BLE", "Failed to create BLE service. Invalid bluetoothType: %d", Config.bluetooth.type);
        }
    }

    void sendToLoRa() {
        if (!sendBleToLoRa) {
            return;
        }

        if (!Config.acceptOwnFrameFromTNC && BLEToLoRaPacket.indexOf("::") == -1) {
            String sender = BLEToLoRaPacket.substring(0,BLEToLoRaPacket.indexOf(">"));

            if (sender == currentBeacon->callsign) {
                BLEToLoRaPacket = "";
                sendBleToLoRa = false;
                return;
            }
        }

        logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE Tx", "%s", BLEToLoRaPacket.c_str());
        displayShow("BLE Tx >>", "", BLEToLoRaPacket, 1000);
        LoRa_Utils::sendNewPacket(BLEToLoRaPacket);
        BLEToLoRaPacket = "";
        sendBleToLoRa = false;
    }

    void txBLE(uint8_t p) {
        pCharacteristicTx->setValue(&p,1);
        pCharacteristicTx->notify();
        delay(3);
    }

    void txToPhoneOverBLE(const String& frame) {
        if (Config.bluetooth.type == 0) { // AX25 KISS
            const String kissEncoded = AX25_Utils::encodeKISS(frame);

            const char* t = kissEncoded.c_str();
            int length = kissEncoded.length();

            const int CHUNK_SIZE = 64;

            for (int i = 0; i < length; i += CHUNK_SIZE) {
                int chunkSize = (length - i < CHUNK_SIZE) ? (length - i) : CHUNK_SIZE;
                
                uint8_t* chunk = new uint8_t[chunkSize];
                memcpy(chunk, t + i, chunkSize);

                pCharacteristicTx->setValue(chunk, chunkSize);
                pCharacteristicTx->notify();

                delete[] chunk;

                delay(200);
            }
        } else { // TNC2
            for(int n = 0; n < frame.length(); n++) {
                txBLE(frame[n]);
            }

            txBLE('\n');
        }   
    }

    void sendToPhone(const String& packet) {
        if (!packet.isEmpty() && bluetoothConnected) {
            logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "BLE Rx", "%s", packet.c_str());
            String receivedPacketString = "";
            for (int i = 0; i < packet.length(); i++) {
                receivedPacketString += packet[i];
            }
            txToPhoneOverBLE(receivedPacketString);     
        }
    }

}