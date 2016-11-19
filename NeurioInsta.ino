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
NTPClient timeClient(ntpUDP, "1.openwrt.pool.ntp.org");

IPAddress neurio_ip;
WiFiClient neurio_client;

IPAddress vera_ip;
WiFiClient vera_client;

//US Eastern Time Zone
TimeChangeRule usEdt = {"EDT", Second, Sun, Mar, 2, -240}; //UTC - 4 hours
TimeChangeRule usEst = {"EST", First, Sun, Nov, 2, -300};  //UTC - 5 hours
Timezone usEastern(usEdt, usEst);

int load_time = 0;
int consumption = 0;
int max_pw_read = 0;
bool is_active = true;
time_t local_trip = 0;

void setup() {
  // Set IPs from strings
  neurio_ip.fromString(NEURIO_IP);
  vera_ip.fromString(VERA_IP);
  
  Serial.begin(115200);
  delay(100);

  // Connecting to WiFi network
  println();
  println();
  print("Connecting to ");
  println(WIFI_NAME);

  WiFi.hostname(HOSTNAME);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);

  // Set display matrix address
  matrix.begin(MATRIX_ADDR);

  while (WiFi.status() != WL_CONNECTED) {
    print(".");
    showLoading(matrix, load_time);
    load_time++;
    delay(120);
  }

  showReady(matrix);

  println();
  println("WiFi connected");
  print("IP address: ");
  println(String("") + WiFi.localIP());
  print("Mac Address: ");
  println(String("") + WiFi.macAddress());

  setSyncProvider(getNtpTime);
  while(timeStatus() == timeNotSet) { delay(50); }
}

void loop() {
  // Close display when no movement
  time_t utc = now();
  time_t local = usEastern.toLocal(utc);

  print("Current Time : ");
  println(formatTime(local));

  // Up to 5 second before max trip grace - keep active without check
  if (local_trip != 0 && (local_trip + TRIP_DELAY - 5) > local) {
    println("Direct Display");
    showConsumption();
  } else {
    // Check for last trip
    bool connected = vera_client.connected();
    if(!connected) {
      connected = vera_client.connect(vera_ip, VERA_PORT);
    }
    
    if (connected) {
      vera_client.setNoDelay(1);
      vera_client.print(String("GET ") + VERA_TRIP_URL + " HTTP/1.1\r\n" +
                      "Host: " + VERA_IP + ":" + VERA_PORT + "\r\n" +
                      "Connection: keep-alive\r\n\r\n");
      String last_trip = getBodyResponse(vera_client);
      
      if(last_trip == "TIMEOUT") {
        // Vera not responding - kee previous state
        println("Vera Response Failed");
        if(is_active) {
          showConsumption();
        }
      } else {
        time_t trip = last_trip.toInt();
        local_trip = usEastern.toLocal(trip);

        print("Last Trip: ");
        println(formatTime(local_trip));

        if((local_trip + TRIP_DELAY) > local) {
          println("Display ON");
          is_active = true;
          showConsumption();
        } else {
          println("Display OFF");
          is_active = false;
          matrix.clear();
          matrix.writeDisplay();
        }
      }
    } else {
      // Vera not responding - kee previous state
      println("Vera Connection Failed");
      if(is_active) {
        showConsumption();
      }
    }
  }

  println();
  delay(250);
}

void showConsumption(){
  bool connected = false;
  
  connected = neurio_client.connected();
  if(!connected){
    connected = neurio_client.connect(neurio_ip, NEURIO_PORT);
  }
  
  if (connected) {
    neurio_client.setNoDelay(1);
    neurio_client.print(String("GET /current-sample ") +
                       "HTTP/1.1\r\n" +
                       "Host: " + NEURIO_IP + "\r\n" +
                       "Connection: keep-alive\r\n\r\n");
                 
    // Read all the lines of the reply from server and print them to Serial
    String response = getJsonBodyResponse(neurio_client);

    StaticJsonBuffer<2048> jsonBuffer;
    JsonObject& sample = jsonBuffer.parseObject(response);
    
    if (!sample.success()) {
      println("JSON parsing failed!");
      println("Response body :");
      println(response);
      return;
    }
    consumption = sample["channels"][2]["p_W"];

    // Track maximum consumption peak
    if(consumption > max_pw_read) {
      max_pw_read = consumption;
    }
    println(String("Current Power : ") + consumption + "kw");
    println(String("Maximum Power : ") + max_pw_read + "kw");
  } else {
    println("Neurio connection failed");
  }

  // Display current consumption & set brightness relative to maximum consumption
  matrix.print(consumption);
  matrix.setBrightness((float)consumption / max_pw_read * 15);
  matrix.writeDisplay();
}

// Get all client body response
String getJsonBodyResponse(WiFiClient client){
  while(client.available()){
    client.setTimeout(50);
    String line = client.readStringUntil('\r');
    // Get only the JSON body, skip headers
    if(line.startsWith("\n{")) {
      return line;
    }
  }
  return "Response body not found";
}

String getBodyResponse(WiFiClient client){
  int max_delay = 5;
  while(!client.available()){
    max_delay--;
    if(max_delay == 0) {
      return "TIMEOUT";
    }
    print(".");
    delay(5);
  }

  while(client.available()){
    client.setTimeout(50);
    String line = client.readStringUntil('\r');
    // Look for headers end
    if(line == "\n") {
      client.readStringUntil('\n');
      String response = client.readStringUntil('\n');
      return response;
    }
  }
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

String formatTime(time_t t) {
  return String("") + hour(t) + (":") + minute(t) + ":" + second(t);
}

// Return the time from the Ntp time client
time_t getNtpTime(){
  timeClient.update();
  return timeClient.getEpochTime();
}

// Debug output methods
void print(String str){
  #ifdef DEBUG
  Serial.print(str);
  #endif
}

void println(){
  #ifdef DEBUG
  Serial.println();
  #endif
}

void println(String str){
  #ifdef DEBUG
  Serial.println(str);
  #endif
}
