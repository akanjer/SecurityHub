#include <Arduino.h>
#include <Wire.h>
#include <WiFiClient.h>
#include <ESP8266WiFi.h>
#include <SpritzCipher.h>
#include <RF24.h>
#include <SSD1306.hpp>
#include <SSD1306Ui.hpp>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"
#include <SD.h>
#include <NtpClientLib.h>

enum DoorState
{
    DoorStateClosed,
    DoorStateOpen
};

struct radioPacket
{
    uint16_t magicNumber;
    uint8_t doorState;
    uint8_t sensorID;
    uint16_t vcc;
    uint8_t padding[26];
};

const uint16_t kMagicNumber = 21212;             // Change as you wish
static const char encKey[] = "kRfxZE9WRMzsX5ns"; // Change as you wish

const uint8_t PIN_RF24_CHIP_ENABLE = D5;
const uint8_t PIN_RF24_CHIP_SELECT = D8;

SSD1306 display(0x3c, D2, D1);
SSD1306Ui ui( &display );

uint8_t writePipe[] = "1Pipa";

const char WIFI_SSID[] PROGMEM = "";
const char WIFI_PASSWORD[] PROGMEM = "";

                // use 8883 for SSL
#define AIO_USERNAME    "" // Adafruit.io username goes here
#define AIO_KEY    "" // Adafruit.io key goes here
#define AIO_SERVERPORT  1883

const char MQTT_SERVER[] PROGMEM    = "io.adafruit.com";
const char MQTT_USERNAME[] PROGMEM  = AIO_USERNAME;
const char MQTT_PASSWORD[] PROGMEM  = AIO_KEY;

WiFiClient client;
// Setup the MQTT client class by passing in the WiFi client and MQTT server and login details.
Adafruit_MQTT_Client mqtt(&client, MQTT_SERVER, AIO_SERVERPORT, MQTT_USERNAME, MQTT_PASSWORD);;

const char MOVEMENT_FEED[] PROGMEM = AIO_USERNAME "/feeds/insertYourFeed";

Adafruit_MQTT_Publish movementFeed = Adafruit_MQTT_Publish(&mqtt, MOVEMENT_FEED);

void MQTT_connect();
bool publishDoorState(uint8_t doorID, uint8_t doorState);
void decryptPacket(radioPacket *packet, const uint8_t packetLen, const uint8_t *key, uint16_t keyLen, radioPacket *packetOut);
void processPacket(radioPacket *packet);
void logToFile(String &eventTime, uint8_t doorState, bool successfullyPublished);

RF24 radio = RF24(PIN_RF24_CHIP_ENABLE, PIN_RF24_CHIP_SELECT);

const uint8_t PIN_SD_CHIP_SELECT = D0;

ntpClient *ntp;

void setupRadio()
{
    radio.begin();
    radio.setPALevel(RF24_PA_HIGH);
    radio.setDataRate(RF24_250KBPS);
    radio.openReadingPipe(1,writePipe);
    radio.startListening();
}

void connectToWIFI()
{
    Serial.print(F("Connecting to "));
    Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");

    Serial.println(F("WiFi connected"));
    Serial.println(F("IP address: "));
    Serial.println(WiFi.localIP());
}

bool setupNTP()
{
    ntp = ntpClient::getInstance("0.de.pool.ntp.org", 1);
    ntp->setInterval(15, 43000); // OPTIONAL. Set sync interval
    return ntp->begin();
}

void setup()
{
    Serial.begin(115200);

    connectToWIFI();

    setupRadio();

    ui.init();
    display.flipScreenVertically();

    if (!SD.begin(PIN_SD_CHIP_SELECT))
    {
        Serial.println(F("Could not init SD card"));
    }

    if (!setupNTP())
    {
        Serial.println(F("Could not init NTP"));
    }
}

void printPacket(radioPacket *packet)
{
    Serial.print(F("Magic number"));
    Serial.println(packet->magicNumber);
    Serial.print(F("Door state: "));
    Serial.println(packet->doorState);
    Serial.print(F("SensorID: "));
    Serial.println(packet->sensorID);
    Serial.print(F("VCC: "));
    Serial.println(packet->vcc);
    Serial.print(F("packetSize: "));
    Serial.println(sizeof(radioPacket));
    Serial.println();
}

void loop()
{
    if( radio.available())
    {
        radioPacket packet;
        // Variable for the received timestamp
        while (radio.available())
        {                                   // While there is data ready
            radio.read(&packet, sizeof(radioPacket));             // Get the payload
        }

        radioPacket decryptedPacket;
        decryptPacket(&packet, sizeof(radioPacket), (const uint8_t*)encKey, strlen(encKey), &decryptedPacket);

        Serial.println(F("crypt:"));
        printPacket(&decryptedPacket);
        processPacket(&decryptedPacket);
    }
}

void processPacket(radioPacket *packet)
{
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_16);

    if (packet->magicNumber == kMagicNumber)
    {
        display.drawStringMaxWidth(0, 0, 128, "SID: " + String(packet->sensorID) + " V:" + String(packet->vcc));
        display.drawStringMaxWidth(0, 32, 128, "Status: " + String(packet->doorState ? "OPEN" : "CLOSED"));

        MQTT_connect();
        bool publishSuc = publishDoorState(packet->sensorID, packet->doorState);
        String currentTime = ntp->getTimeString();
        logToFile(currentTime, packet->doorState, publishSuc);
    }
    else
    {
        display.drawStringMaxWidth(0, 0, 128, F("Wrong magic number!"));
    }

    display.display();
}

bool publishDoorState(uint8_t doorID, uint8_t doorState)
{
    Serial.println(F("\nPublishing door state..."));
    String stateToSend = (doorState ? F("OPEN") : F("CLOSED"));
    char stateToSendChar[32];
    stateToSend.toCharArray(stateToSendChar, 32);

    Serial.println(stateToSendChar);

    bool publishSuc = movementFeed.publish(stateToSendChar);

    if (publishSuc)
    {
        Serial.println(F("Published!"));
    }
    else
    {
        Serial.println(F("Publish failed"));
    }

    return publishSuc;
}

void logToFile(String &eventTime, uint8_t doorState, bool successfullyPublished)
{
    File logFile = SD.open("aLog.txt", FILE_WRITE);

    if (logFile)
    {
        Serial.println(F("File opened, saving:"));
        String stateToSend = eventTime;
        stateToSend += F("\t");
        stateToSend += (doorState ? F("OPEN") : F("CLOSED"));
        stateToSend += F("\t publishSuc: ");
        stateToSend += successfullyPublished ? F("YES") : F("NO");
        logFile.println(stateToSend);
        logFile.close();

        Serial.println(stateToSend);
    }
    else
    {
        Serial.println(F("Could not open file..."));
    }

}

void MQTT_connect()
{
    if (!mqtt.connected())
    {
        Serial.print(F("Connecting to MQTT... "));

        uint8_t retries = 3;
        int8_t ret;
        while ((ret = mqtt.connect()) != 0) { // connect will return 0 for connected
            Serial.println(mqtt.connectErrorString(ret));
            Serial.println(F("Retrying MQTT connection in 5 seconds..."));
            mqtt.disconnect();
            delay(5000);  // wait 5 seconds
            retries--;
            if (retries == 0) {
                // basically die and wait for WDT to reset me
                while (1);
            }
        }
        Serial.println(F("MQTT Connected!"));
    }
}

void decryptPacket(radioPacket *packet, const uint8_t packetLen, const uint8_t *key, uint16_t keyLen, radioPacket *packetOut)
{
    spritz_ctx s_ctx;
    spritz_setup(&s_ctx, key, keyLen);
    spritz_crypt(&s_ctx, (const uint8_t*)packet, packetLen, (uint8_t*)packetOut);
}
