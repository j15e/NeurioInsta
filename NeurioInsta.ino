#include "NeurioInstaEnv.h"

#include <Adafruit_LEDBackpack.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Time.h>
#include <TimeLib.h>
#include <Timezone.h>

Adafruit_7segment matrix = Adafruit_7segment();

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

//US Eastern Time Zone
TimeChangeRule usEdt = {"EDT", Second, Sun, Mar, 2, -240};    //UTC - 4 hours
TimeChangeRule usEst = {"EST", First, Sun, Nov, 2, -300};     //UTC - 5 hours
Timezone timezone(usEdt, usEst);
TimeChangeRule *tcr;
time_t utc, local;

int load_time = 0;
int max_pw_read = 0;

void setup() {
  Serial.begin(115200);
  delay(100);

  // Connecting to WiFi network
  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(WIFI_NAME);

  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);

  // Set display matrix address
  matrix.begin(MATRIX_ADDR);

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    showLoading(matrix, load_time);
    load_time++;
    delay(250);
  }

  showReady(matrix);

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  setSyncProvider(getNtpTime);
  setSyncInterval(60);
}

void loop() {
  WiFiClient client;
  const uint16_t httpPort = 80;

  if (!client.connect(NEURIO_IP, httpPort)) {
    Serial.println("connection failed");
    return;
  }

  // This will send the request to the server
  client.print(String("GET /current-sample") + " HTTP/1.1\r\n" +
               "Host: " + NEURIO_IP + "\r\n" +
               "Connection: close\r\n\r\n");

  // Read all the lines of the reply from server and print them to Serial
  String response = "";
  while(client.available()){
    String line = client.readStringUntil('\r');
    // Get only the JSON body, skip headers
    if(line.startsWith("\n{")) {
      response = line;
    }
  }

  StaticJsonBuffer<2048> jsonBuffer;
  JsonObject& sample = jsonBuffer.parseObject(response);

  if (!sample.success()) {
    Serial.println("JSON parsing failed!");
    Serial.println("Response body :");
    Serial.println(response);
    return;
  }

  int consumption = sample["channels"][2]["p_W"];

  // Track maximum consumption peak
  if(consumption > max_pw_read) {
    max_pw_read = consumption;
  }

  // Display current consumption & set brightness relative to maximum consumption
  matrix.setBrightness((float)consumption / max_pw_read * 15);

  // Close display between midnight & 6 AM
  utc = now();
  local = timezone.toLocal(utc, &tcr);
  int current_hour = hour(local);
  if(current_hour >= NIGHT_START || current_hour < NIGHT_STOP) {
    matrix.clear();
  } else {
    matrix.print(consumption);
  }

  matrix.writeDisplay();
  delay(200);
}

// Show loading screen
// [=   ]
// [ =  ]
// [  = ]
// [   =]
void showLoading(Adafruit_7segment matrix, int load_time){
  int displayDigit = (load_time % 4);
  int digit = 0;

  // Skip display colon
  if(displayDigit > 1) displayDigit++;

  while(digit < 5) {
    if(digit == displayDigit) {
      matrix.writeDigitRaw(digit, 0x49);
    } else {
      matrix.writeDigitRaw(digit, 0x00);
    }
    digit++;
   }

   matrix.writeDisplay();
}

// Show ready screen [====]
void showReady(Adafruit_7segment matrix){
  matrix.writeDigitRaw(0, 0x49);
  matrix.writeDigitRaw(1, 0x49);
  matrix.writeDigitRaw(3, 0x49);
  matrix.writeDigitRaw(4, 0x49);
  matrix.writeDisplay();
}

// Return the time from the Ntp time client
time_t getNtpTime(){
  timeClient.update();
  return timeClient.getEpochTime();
}
