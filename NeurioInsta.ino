#include "NeurioInstaEnv.h"

#include <Adafruit_LEDBackpack.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Time.h>
#include <TimeLib.h>
#include <Timezone.h>

Adafruit_7segment matrix1 = Adafruit_7segment();
Adafruit_7segment matrix2 = Adafruit_7segment();

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

unsigned int load_time = 0;
unsigned int max_pw_read = 0;
unsigned long day_start_kwh = 0;
time_t day_start_time = 0;
time_t local_trip = 0;
bool is_active = true;

#define NEURIO_GET_QUERY_P1 "GET /current-sample HTTP/1.1\r\nHost: "
#define NEURIO_GET_QUERY_P2 "\r\nConnection: keep-alive\r\n\r\n"
#define NEURIO_GET_QUERY NEURIO_GET_QUERY_P1 NEURIO_IP NEURIO_GET_QUERY_P2

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
  matrix1.begin(MATRIX1_ADDR);
  matrix2.begin(MATRIX2_ADDR);

  while (WiFi.status() != WL_CONNECTED) {
    print(".");
    showLoading(matrix1, load_time);
    showLoading(matrix2, load_time);
    load_time++;
    delay(120);
  }

  showReady(matrix1);
  showReady(matrix2);

  println();
  println("WiFi connected");
  print("IP address: ");
  Serial.println(WiFi.localIP());
  print("Mac Address: ");
  Serial.println(WiFi.macAddress());

  setSyncProvider(getNtpTime);
  while(timeStatus() == timeNotSet) { delay(50); }
}

void loop() {
  // Close display when no movement
  time_t local = usEastern.toLocal(now());
  bool show = false;

  print("Current Time: ");
  printTime(local);

  // Up to 5 second before max trip grace - keep active without check
  if (local_trip != 0 && (local_trip + TRIP_DELAY - 5) > local) {
    println("Direct Display");
    show = true;
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
        show = is_active;
      } else {
        time_t trip = last_trip.toInt();
        local_trip = usEastern.toLocal(trip);

        print("Last Trip: ");
        printTime(local_trip);

        if((local_trip + TRIP_DELAY) > local) {
          println("Display ON");
          is_active = true;
          show = true;
        }
      }
    } else {
      // Vera not responding - kee previous state
      println("Vera Connection Failed");
      show = is_active;
    }
  }

  // Get & show consumption
  if(show) {
    getConsumption(true);
  // Get required for consumption of the day
  } else if(day_start_time == 0 || day(day_start_time) < day(local)) {
    getConsumption(false);
  } else {
    println("Display OFF");
    is_active = false;
    matrix1.clear();
    matrix2.clear();
    matrix1.writeDisplay();
    matrix2.writeDisplay();
  }

  println();
  delay(250);
}

void getConsumption(bool show){
  bool connected = neurio_client.connected();
  time_t current_time = usEastern.toLocal(now());
  unsigned long consumption_kwh = 0;
  unsigned int consumption_pw = 0;
  unsigned int today_imported_wh = 0;
  
  if(!connected){
    connected = neurio_client.connect(neurio_ip, 80);
  }
  
  if (connected) {
    neurio_client.setNoDelay(1);
    neurio_client.print(NEURIO_GET_QUERY);
                 
    // Read all the lines of the reply from server and print them to Serial
    String response = getJsonBodyResponse(neurio_client);
  
    DynamicJsonBuffer jsonBuffer;
    JsonObject& sample = jsonBuffer.parseObject(response);
    
    if (!sample.success()) {
      println("JSON parsing failed - response body was :");
      println(response);
      return;
    }
    
    consumption_kwh = ws2wh(sample["channels"][2]["eImp_Ws"]);
    consumption_pw = sample["channels"][2]["p_W"];

    // Track maximum consumption peak
    if(consumption_pw > max_pw_read) {
      max_pw_read = consumption_pw;
    }

    // Track consumption over time
    if(day_start_time == 0 || day(day_start_time) != day(current_time)) {
      day_start_kwh = consumption_kwh;
      day_start_time = current_time;
    }
    
    today_imported_wh = (consumption_kwh - day_start_kwh);

    #ifdef DEBUG
    char buffer[28];
    sprintf(buffer, "Current Power : %dkW", consumption_pw);
    println(buffer);
    sprintf(buffer, "Maximum Power : %dkW", max_pw_read);
    println(buffer);
    sprintf(buffer, "Imported Power : %dWh", today_imported_wh);
    println(buffer);
    #endif
  } else {
    println("Neurio connection failed");
  }

  // Stop here if display off
  if(!show) return;

  // Display current consumption & set brightness relative to maximum consumption
  float brightness = (float)consumption_pw / max_pw_read * 15;

  printPw2Matrix(consumption_pw, matrix1);
  matrix1.setBrightness(brightness);
  matrix1.writeDisplay();
  
  printPw2Matrix(today_imported_wh, matrix2);
  matrix2.setBrightness(brightness);
  matrix2.writeDisplay();
}

// Get all client body response
String getJsonBodyResponse(WiFiClient &client){
  while(client.available()){
    client.setTimeout(200);
    String line = client.readStringUntil('\r');
    // Get only the JSON body, skip headers
    if(line.startsWith("\n{")) {
      return line;
    }
  }
  return "NOT_FOUND";
}

String getBodyResponse(WiFiClient &client){
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
    client.setTimeout(200);
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
void showLoading(Adafruit_7segment &matrix, int load_time){
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
void showReady(Adafruit_7segment &matrix){
  matrix.writeDigitRaw(0, 0x49);
  matrix.writeDigitRaw(1, 0x49);
  matrix.writeDigitRaw(3, 0x49);
  matrix.writeDigitRaw(4, 0x49);
  matrix.writeDisplay();
}

// Display power on matrix & show as thousands if required
void printPw2Matrix(int pw, Adafruit_7segment &matrix){
  if(pw > 9999) {
    matrix.print((float)pw / 1000, 3);
  } else {
    matrix.print(pw);
  }
}

// Convert too long string of watt-second to watt-hour 
unsigned long ws2wh(const char *ws) {
  int ws_len = strlen(ws);
  
  if(ws_len > 9){
    // Truncate what is too long
    char truncated[10];
    strncpy(truncated, ws, 9);
    // Divide the rest
    return atol(truncated) / pow(10, 2 - (ws_len - 9)) / 36;
  } else {
    return atol(ws) / 3600;
  }
}

// Return the time from the Ntp time client
time_t getNtpTime(){
  timeClient.update();
  return timeClient.getEpochTime();
}

void printTime(time_t t) {
  #ifdef DEBUG
  char format[10];
  sprintf(format, "%u:%u:%u", hour(t), minute(t), second(t));
  Serial.println(format);
  #endif
}

// Debug output methods
void print(String &str){
  #ifdef DEBUG
  Serial.print(str);
  #endif
}

void print(const char *str){
  #ifdef DEBUG
  Serial.print(str);
  #endif
}

void println(){
  #ifdef DEBUG
  Serial.println();
  #endif
}

void println(String &str){
  #ifdef DEBUG
  Serial.println(str);
  #endif
}

void println(const char *str){
  #ifdef DEBUG
  Serial.println(str);
  #endif
}
