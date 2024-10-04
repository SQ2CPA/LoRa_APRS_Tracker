#include <TinyGPS++.h>
#include <SPIFFS.h>
#include "APRSPacketLib.h"
#include "station_utils.h"
#include "battery_utils.h"
#include "configuration.h"
#include "boards_pinout.h"
#include "power_utils.h"
#include "sleep_utils.h"
#include "lora_utils.h"
#include "bme_utils.h"
#include "display.h"
#include "logger.h"
#include "ble_utils.h"

extern Configuration        Config;
extern Beacon               *currentBeacon;
extern logging::Logger      logger;
extern TinyGPSPlus          gps;
extern uint8_t              myBeaconsIndex;
extern uint8_t              loraIndex;

extern uint32_t             lastTx;
extern uint32_t             lastTxTime;

extern bool                 sendUpdate;

extern double               currentHeading;
extern double               previousHeading;

extern double               lastTxLat;
extern double               lastTxLng;
extern double               lastTxDistance;

extern bool                 miceActive;
extern bool                 smartBeaconActive;
extern bool                 winlinkCommentState;

extern int                  wxModuleType;
extern bool                 wxModuleFound;
extern bool                 gpsIsActive;
extern bool                 gpsShouldSleep;


bool	    sendStandingUpdate      = false;
uint8_t     updateCounter           = 100;


bool        sendStartTelemetry      = true;
uint32_t    lastTelemetryTx         = 0;
uint32_t    telemetryTx             = millis();

uint32_t    lastDeleteListenedTracker;

struct nearTracker {
    String      callsign;
    float       distance;
    int         course;
    uint32_t    lastTime;
};

nearTracker nearTrackers[4];


namespace STATION_Utils {

    void nearTrackerInit() {
        for (int i = 0; i < 4; i++) {
            nearTrackers[i].callsign    = "";
            nearTrackers[i].distance    = 0.0;
            nearTrackers[i].course      = 0;
            nearTrackers[i].lastTime    = 0;
        }
    }

    const String getNearTracker(uint8_t position) {
        if (nearTrackers[position].callsign == "") {
            return "";
        } else {
            return nearTrackers[position].callsign + "> " + String(nearTrackers[position].distance,2) + "km " + String(nearTrackers[position].course);
        }
    }

    void deleteListenedTrackersbyTime() {
        for (int a = 0; a < 4; a++) {                       // clean nearTrackers[] after time
            if (nearTrackers[a].callsign != "" && (millis() - nearTrackers[a].lastTime > Config.rememberStationTime * 60 * 1000)) {
                nearTrackers[a].callsign    = "";
                nearTrackers[a].distance    = 0.0;
                nearTrackers[a].course      = 0;
                nearTrackers[a].lastTime    = 0;
            }
        }

        for (int b = 0; b < 4 - 1; b++) {
            for (int c = 0; c < 4 - b - 1; c++) {
                if (nearTrackers[c].callsign == "") {       // get "" nearTrackers[] at the end
                    nearTracker temp = nearTrackers[c];
                    nearTrackers[c] = nearTrackers[c + 1];
                    nearTrackers[c + 1] = temp;
                }
            }
        }
        lastDeleteListenedTracker = millis();
    }

    void checkListenedTrackersByTimeAndDelete() {
        if (millis() - lastDeleteListenedTracker > Config.rememberStationTime * 60 * 1000) {
            deleteListenedTrackersbyTime();
        }
    }

    void orderListenedTrackersByDistance(const String& callsign, float distance, float course) {   
        bool shouldSortbyDistance = false;
        bool callsignInNearTrackers = false;

        for (int a = 0; a < 4; a++) {                       // check if callsign is in nearTrackers[]
            if (nearTrackers[a].callsign == callsign) {
                callsignInNearTrackers  = true;
                nearTrackers[a].lastTime = millis();        // update listened millis()
                if (nearTrackers[a].distance != distance) { // update distance if needed
                    nearTrackers[a].distance    = distance;
                    shouldSortbyDistance        = true;
                }
                break;           
            }
        }
    
        if (!callsignInNearTrackers) {                      // callsign not in nearTrackers[]
            for (int b = 0; b < 4; b++) {                   // if nearTrackers[] is available
                if (nearTrackers[b].callsign == "") {
                    shouldSortbyDistance        = true;
                    nearTrackers[b].callsign    = callsign;
                    nearTrackers[b].distance    = distance;
                    nearTrackers[b].course      = int(course);
                    nearTrackers[b].lastTime    = millis();
                    break;
                }
            }

            if (!shouldSortbyDistance) {                    // if no more nearTrackers[] available , it compares distances to move and replace
                for (int c = 0; c < 4; c++) {
                    if (nearTrackers[c].distance > distance) {
                        for (int d = 3; d > c; d--) {
                            nearTrackers[d] = nearTrackers[d - 1];
                        }
                        nearTrackers[c].callsign    = callsign;
                        nearTrackers[c].distance    = distance;
                        nearTrackers[c].course      = int(course);
                        nearTrackers[c].lastTime    = millis();
                        break;
                    }
                }
            }
        }

        if (shouldSortbyDistance) {                         // sorts by distance (only nearTrackers[] that are not "")
            for (int f = 0; f < 4 - 1; f++) {
                for (int g = 0; g < 4 - f - 1; g++) {
                    if (nearTrackers[g].callsign != "" && nearTrackers[g + 1].callsign != "") {
                        if (nearTrackers[g].distance > nearTrackers[g + 1].distance) {
                            nearTracker temp = nearTrackers[g];
                            nearTrackers[g] = nearTrackers[g + 1];
                            nearTrackers[g + 1] = temp;
                        }
                    }
                }
            }
        }
    }

    void checkStandingUpdateTime() {
        if (!sendUpdate && lastTx >= Config.standingUpdateTime * 60 * 1000) {
            sendUpdate = true;
            sendStandingUpdate = true;
            if (!gpsIsActive) {
                SLEEP_Utils::gpsWakeUp();
            }
        }
    }

    void sendBeacon(uint8_t type) {
        if (sendStartTelemetry && Config.battery.sendVoltage && Config.battery.voltageAsTelemetry) {                
            String sender = currentBeacon->callsign;
            for (int i = sender.length(); i < 9; i++) {
                sender += ' ';
            }
            String basePacket = currentBeacon->callsign;
            basePacket += ">APLRT1";
            if (Config.path != "") {
                basePacket += ",";
                basePacket += Config.path;
            }
            basePacket += "::";
            basePacket += sender;
            basePacket += ":";

            String tempPacket = basePacket;
            tempPacket += "EQNS.0,0.01,0";
            LoRa_Utils::sendNewPacket(tempPacket);
            delay(3000);

            tempPacket = basePacket;
            tempPacket += "UNIT.VDC";
            LoRa_Utils::sendNewPacket(tempPacket);
            delay(3000);

            tempPacket = basePacket;
            tempPacket += "PARM.V_Batt";
            LoRa_Utils::sendNewPacket(tempPacket);
            delay(3000);
            sendStartTelemetry = false;
        }

        String packet;
        if (Config.bme.sendTelemetry && wxModuleFound && type == 1) { // WX
            packet = APRSPacketLib::generateGPSBeaconPacket(currentBeacon->callsign, "APLRT1", Config.path, "/", APRSPacketLib::encodeGPS(gps.location.lat(),gps.location.lng(), gps.course.deg(), 0.0, currentBeacon->symbol, Config.sendAltitude, gps.altitude.feet(), sendStandingUpdate, "Wx"));
            if (wxModuleType != 0) {
                packet += BME_Utils::readDataSensor(0);
            } else {
                packet += ".../...g...t...";
            }            
        } else {
            String path = Config.path;
            if (gps.speed.kmph() > 200 || gps.altitude.meters() > 9000) {   // avoid plane speed and altitude
                path = "";
            }
            if (miceActive) {
                packet = APRSPacketLib::generateMiceGPSBeacon(currentBeacon->micE, currentBeacon->callsign, currentBeacon->symbol, currentBeacon->overlay, path, gps.location.lat(), gps.location.lng(), gps.course.deg(), gps.speed.knots(), gps.altitude.meters());
            } else {
                packet = APRSPacketLib::generateGPSBeaconPacket(currentBeacon->callsign, "APLRT1", path, currentBeacon->overlay, APRSPacketLib::encodeGPS(gps.location.lat(),gps.location.lng(), gps.course.deg(), gps.speed.knots(), currentBeacon->symbol, Config.sendAltitude, gps.altitude.feet(), sendStandingUpdate, "GPS"));
            }
        }
        String comment;
        int sendCommentAfterXBeacons;
        if (winlinkCommentState || Config.battery.sendVoltageAlways) {
            if (winlinkCommentState) comment = " winlink";
            sendCommentAfterXBeacons = 1;
        } else {
            comment = currentBeacon->comment;
            sendCommentAfterXBeacons = Config.sendCommentAfterXBeacons;
        }
        String batteryVoltage = POWER_Utils::getBatteryInfoVoltage();
        #if defined(BATTERY_PIN) && !defined(HAS_AXP192) && !defined(HAS_AXP2101)
            BATTERY_Utils::checkLowVoltageAndSleep(batteryVoltage.toFloat());
        #endif
        if (Config.battery.sendVoltage && !Config.battery.voltageAsTelemetry) {
            String batteryChargeCurrent = POWER_Utils::getBatteryInfoCurrent();
            #if defined(HAS_AXP192)
                comment += " Bat=";
                comment += batteryVoltage;
                comment += "V (";
                comment += batteryChargeCurrent;
                comment += "mA)";
            #elif defined(HAS_AXP2101)
                comment += " Bat=";
                comment += String(batteryVoltage.toFloat(),2);
                comment += "V (";
                comment += batteryChargeCurrent;
                comment += "%)";
            #elif defined(BATTERY_PIN) && !defined(HAS_AXP192) && !defined(HAS_AXP2101)
                comment += " Bat=";
                comment += String(batteryVoltage.toFloat(),2);
                comment += "V";
                comment += BATTERY_Utils::getPercentVoltageBattery(batteryVoltage.toFloat());
                comment += "%";
            #endif
        }
        if (comment != "" || (Config.battery.sendVoltage && Config.battery.voltageAsTelemetry)) {
            updateCounter++;
            if (updateCounter >= sendCommentAfterXBeacons) {
                if (comment != "") packet += comment;
                if (Config.battery.sendVoltage && Config.battery.voltageAsTelemetry) packet += BATTERY_Utils::generateEncodedTelemetry(batteryVoltage.toFloat());
                updateCounter = 0;
            }
        }
        #ifdef HAS_TFT
            cleanTFT();
        #endif
        displayShow("<<< TX >>>", "", packet,100);
        LoRa_Utils::sendNewPacket(packet);
        
        if (Config.bluetooth.type == 0 || Config.bluetooth.type == 2) {
            BLE_Utils::sendToPhone(packet);
        }

        if (smartBeaconActive) {
            lastTxLat       = gps.location.lat();
            lastTxLng       = gps.location.lng();
            previousHeading = currentHeading;
            lastTxDistance  = 0.0;
        }
        lastTxTime  = millis();
        sendUpdate  = false;
        #ifdef HAS_TFT
            cleanTFT(); 
        #endif
        if (currentBeacon->gpsEcoMode) {
            gpsShouldSleep = true;
        }
        #if !defined(HAS_AXP192) && !defined(HAS_AXP2101) && defined(BATTERY_PIN)
            if (batteryVoltage.toFloat() < 3.0) {
                POWER_Utils::shutdown();
            }
        #endif
    }

    void checkTelemetryTx() {
        if (Config.bme.active && Config.bme.sendTelemetry && sendStandingUpdate) {
            lastTx = millis() - lastTxTime;
            telemetryTx = millis() - lastTelemetryTx;
            if ((lastTelemetryTx == 0 || telemetryTx > 10 * 60 * 1000) && lastTx > 10 * 1000) {
                sendBeacon(1);
                lastTelemetryTx = millis();
            }
        }
    }

    void saveIndex(uint8_t type, uint8_t index) {
        String filePath;
        if (type == 0) {
            filePath = "/callsignIndex.txt";
        } else {
            filePath = "/freqIndex.txt";
        }
        File fileIndex = SPIFFS.open(filePath, "w");
        if(!fileIndex) {
            return;
        }
        String dataToSave = String(index);
        if (fileIndex.println(dataToSave)) {
            if (type == 0) {
                logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "Main", "New Callsign Index saved to SPIFFS");
            } else {
                logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "Main", "New Frequency Index saved to SPIFFS");
            }
        } 
        fileIndex.close();
    }

    void loadIndex(uint8_t type) {
        String filePath;
        if (type == 0) {
            filePath = "/callsignIndex.txt";
        } else {
            filePath = "/freqIndex.txt";
        }
        File fileIndex = SPIFFS.open(filePath);
        if(!fileIndex) {
            return;
        } else {
            while (fileIndex.available()) {
                String firstLine = fileIndex.readStringUntil('\n');
                if (type == 0) {
                    myBeaconsIndex = firstLine.toInt();
                    logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "Main", "Callsign Index: %s", firstLine);
                } else {
                    loraIndex = firstLine.toInt();
                    logger.log(logging::LoggerLevel::LOGGER_LEVEL_DEBUG, "LoRa", "LoRa Freq Index: %s", firstLine);
                }
            }
            fileIndex.close();
        }
    }

}