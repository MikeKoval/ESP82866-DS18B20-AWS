#include <Arduino.h>
#include <Stream.h>

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>

#include <OneWire.h>
#include <DallasTemperature.h>

//AWS
#include "sha256.h"
#include "Utils.h"

//WEBSockets
#include <Hash.h>
#include <WebSocketsClient.h>

//MQTT PAHO
#include <SPI.h>
#include <IPStack.h>
#include <Countdown.h>
#include <MQTTClient.h>

#include <ArduinoJson.h>



//AWS MQTT Websocket
#include "Client.h"
#include "AWSWebSocketClient.h"
#include "CircularByteBuffer.h"

// Data wire is plugged into pin D1 on the ESP8266 12-E - GPIO 5
#define ONE_WIRE_BUS D1

#define RELAY_PIN D5 //GPIO2 - Led auf dem Modul selbst

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature DS18B20(&oneWire);
char temperatureCString[6];

//AWS IOT config, change these:
char wifi_ssid[]       = "your-ssid";
char wifi_password[]   = "your-password";
char aws_endpoint[]    = "your-endpoint.iot.eu-west-1.amazonaws.com";
char aws_key[]         = "your-iam-key";
char aws_secret[]      = "your-iam-secret-key";
char aws_region[]      = "eu-west-1";
const char* aws_topic  = "$aws/things/your-device/shadow/update";
int port = 443;

//MQTT config
const int maxMQTTpackageSize = 512;
const int maxMQTTMessageHandlers = 1;

// Variables will change:
int ledState = LOW;             // ledState used to set the LED

// Generally, you should use "unsigned long" for variables that hold time
// The value will quickly become too large for an int to store
unsigned long previousSendMsgMillis = 0;        // will store last time LED was updated
unsigned long previousTempSensMillis = 0;        // will store last time LED was updated
unsigned long currentMillis = 0;

// constants won't change:
//const long interval = 1000;           // interval at which to blink (milliseconds)
const long sendMsgPeriod = 1000 * 60;
const long tempSensPeriod = 1000;

float tempC;
const float tempToSwitchOn = 27;
const float tempToSwitchOff = 30;
boolean isHeatingEnabled = false;


ESP8266WiFiMulti WiFiMulti;

AWSWebSocketClient awsWSclient(1000);

IPStack ipstack(awsWSclient);
MQTT::Client<IPStack, Countdown, maxMQTTpackageSize, maxMQTTMessageHandlers> *client = NULL;

//# of connections
long connection = 0;

//generate random mqtt clientID
char* generateClientID () {
  char* cID = new char[23]();
  for (int i=0; i<22; i+=1)
    cID[i]=(char)random(1, 256);
  return cID;
}

void getTemperature() {
    DS18B20.requestTemperatures(); 
    tempC = DS18B20.getTempCByIndex(0);
    Serial.println(tempC);
    dtostrf(tempC, 2, 2, temperatureCString);
}

//count messages arrived
int arrivedcount = 0;

//callback to handle mqtt messages
void messageArrived(MQTT::MessageData& md)
{
  MQTT::Message &message = md.message;

  Serial.print("Message ");
  Serial.print(++arrivedcount);
  Serial.print(" arrived: qos ");
  Serial.print(message.qos);
  Serial.print(", retained ");
  Serial.print(message.retained);
  Serial.print(", dup ");
  Serial.print(message.dup);
  Serial.print(", packetid ");
  Serial.println(message.id);
  Serial.print("Payload ");
  char* msg = new char[message.payloadlen+1]();
  memcpy (msg,message.payload,message.payloadlen);
  Serial.println(msg);

  StaticJsonBuffer<200> jsonBuffer;

  JsonObject& root = jsonBuffer.parseObject(msg);
  
  delete msg;
}

//connects to websocket layer and mqtt layer
bool connect () {

    if (client == NULL) {
      client = new MQTT::Client<IPStack, Countdown, maxMQTTpackageSize, maxMQTTMessageHandlers>(ipstack);
    } else {

      if (client->isConnected ()) {    
        client->disconnect ();
      }  
      delete client;
      client = new MQTT::Client<IPStack, Countdown, maxMQTTpackageSize, maxMQTTMessageHandlers>(ipstack);
    }


    //delay is not necessary... it just help us to get a "trustful" heap space value
    delay (1000);
    Serial.print (millis ());
    Serial.print (" - conn: ");
    Serial.print (++connection);
    Serial.print (" - (");
    Serial.print (ESP.getFreeHeap ());
    Serial.println (")");

   int rc = ipstack.connect(aws_endpoint, port);

    if (rc != 1)
    {
      Serial.println("error connection to the websocket server");
      return false;
    } else {
      Serial.println("websocket layer connected");
    }


    Serial.println("MQTT connecting");
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 3;
    char* clientID = generateClientID ();
    data.clientID.cstring = clientID;
    rc = client->connect(data);
    delete[] clientID;
    if (rc != 0)
    {
      Serial.print("error connection to MQTT server");
      Serial.println(rc);
      return false;
    }
    Serial.println("MQTT connected");
    return true;
}

//subscribe to a mqtt topic
void subscribe () {
   //subscript to a topic
    int rc = client->subscribe(aws_topic, MQTT::QOS0, messageArrived);
    if (rc != 0) {
      Serial.print("rc from MQTT subscribe is ");
      Serial.println(rc);
      return;
    }
    Serial.println("MQTT subscribed");
}


//send a message to a mqtt topic
void sendmessage () {
    if (currentMillis - previousSendMsgMillis < sendMsgPeriod) {
      return;
    }

    previousSendMsgMillis = currentMillis;
  
    //send a message
    MQTT::Message message;
    char buf[150];
    sprintf(buf, "{\"state\":{\"reported\":{\"temp\": \"%s\", \"isHeatingEnabled\": \"%d\"}, \"desired\":{\"temp\": \"%s\", \"isHeatingEnabled\": \"%d\"}}}", temperatureCString, isHeatingEnabled, temperatureCString, isHeatingEnabled);
    Serial.print(buf);
    message.qos = MQTT::QOS0;
    message.retained = false;
    message.dup = false;
    message.payload = (void*)buf;
    message.payloadlen = strlen(buf)+1;
    int rc = client->publish(aws_topic, message); 
}


void setup() {
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, HIGH);

    configTime(1000, 0, "pool.ntp.org", "time.nist.gov");
    //fill AWS parameters    
        awsWSclient.setAWSRegion(aws_region);
        awsWSclient.setAWSDomain(aws_endpoint);
        awsWSclient.setAWSKeyID(aws_key);
        awsWSclient.setAWSSecretKey(aws_secret);
        awsWSclient.setUseSSL(true);

    //fill with ssid and wifi password

    Serial.begin (115200);
    delay (2000);
    Serial.setDebugOutput(1);

    WiFiMulti.addAP(wifi_ssid, wifi_password);
    Serial.println ("connecting to wifi");
    if(WiFiMulti.run() == WL_CONNECTED) {
        delay(100);
        Serial.print (".");

        Serial.println ("\nconnected");
    
        if (connect ()){
          subscribe ();
        }
    }
}

void loop() {
  currentMillis = millis();

  if (currentMillis - previousTempSensMillis >= tempSensPeriod) {
      // save the last time you blinked the LED
      previousTempSensMillis = currentMillis;

      getTemperature();

      if (tempC <= tempToSwitchOn) {
        isHeatingEnabled = true;
        digitalWrite(RELAY_PIN, LOW);
      }
      if (tempC > tempToSwitchOff) {
        isHeatingEnabled = false;
        digitalWrite(RELAY_PIN, HIGH);
      }
    }

  //keep the mqtt up and running
  if (awsWSclient.connected ()) {    
      client->yield();
      sendmessage ();
  } else {
    //handle reconnection
    if (WiFiMulti.run() == WL_CONNECTED && connect ()){
      subscribe ();      
    }
  }
}
