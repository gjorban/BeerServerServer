#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

// GPIO where the DS18B20 is connected to
const int oneWireBus = 13;     
const int thermostat_relay_pin = 12;

bool compressor_running = false;
bool compressor_wants_to_run = false;

// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(oneWireBus);

// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature sensors(&oneWire);

extern const char *k_WifiSSID;
extern const char *k_WifiPassword;
extern const char *k_HostName;

const long k_TimeBetweenTempChecksMillis = 1000l;
const long k_MinRelayTimeOn = 1 * 60 * 1000; // 1 minute
const long k_MinRelayTimeOff = 2 * 60 * 1000; // 2 minutes

long next_temp_check = 0l;
long next_relay_state_change = 0l;
long last_relay_state_change = 0l;

float current_fridge_temp = 0.0f;
float desired_fridge_temp = 8.0f;

const float k_TempHysteresisCelsius = 1.5f;

void setCompressorState(const long &now, bool force = false);

char scratch[16];

WebServer server(80);

void handle_on_access() {
    server.sendHeader("Connection", "close");

    String output = "<!DOCTYPE html> <html>\n";
    output +="<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">\n";
    output +="<title>G-bot Fridge Control</title>\n";
    output +="<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}\n";

    output +="body{margin-top: 50px;} h1 {color: #444444;margin: 50px auto 30px;} h3 {color: #444444;margin-bottom: 50px;}\n";
    output +="p {font-size: 14px;color: #888;margin-bottom: 10px;}\n";

    output +=".button {display: block;width: 80px;background-color: #3498db;border: none;color: white;padding: 13px 30px;text-decoration: none;font-size: 25px;margin: 0px auto 35px;cursor: pointer;border-radius: 4px;}\n";
    output +=".button-on {background-color: #3498db;}\n";
    output +=".button-on:active {background-color: #2980b9;}\n";
    output +=".button-off {background-color: #34495e;}\n";
    output +=".button-off:active {background-color: #2c3e50;}\n";
    output += "</style></head>";

    output += "<body>";
    output +="<h1>G-Bot Fridge's Web Server</h1>\n";

    output += "<p>Current temp: ";
    dtostrf(current_fridge_temp,5, 2, scratch);
    output += scratch;
    output += "*C</p>\n";
        
    output += "<p>Set temp: ";
    dtostrf(desired_fridge_temp,5, 2, scratch);
    output += scratch;
    output += "*C</p>\n";
    
    output += "<p>Compressor is: ";
    if (compressor_running) {
      output += "on";
    }
    else {
      output += "off";
    }
    output += "</p>\n";

    output += "<p>And has been for (mm:ss): ";

    long now = millis() - last_relay_state_change;
    float seconds = floor(now / 1000.0f);
    float minutes = floor(seconds / 60.0f);
    seconds = (int)seconds % 60;

    dtostrf(minutes,2, 0, scratch);
    output += scratch;
    output += ":";

    dtostrf(seconds,2, 0, scratch);
    output += scratch;
    output += "</p>\n"; 

    if (compressor_wants_to_run != compressor_running)
    {
      output += "<p>The compressor wants to be: ";
      if (desired_fridge_temp) {
        output += "on";
      }
      else {
        output += "off";
      }
      output += "</p>\n";
  
      long timeToNextRelayChange = next_relay_state_change - now;
      seconds = floor(timeToNextRelayChange / 1000.0f);
      minutes = floor(seconds / 60.0f);
      seconds = (int)seconds % 60;
  
      output += "<p>This can happen in (mm:ss): ";
  
      dtostrf(minutes,2, 0, scratch);
      output += scratch;
      output += ":";
  
      dtostrf(seconds,2, 0, scratch);
      output += scratch;
      output += "</p>\n"; 
    }
    
    output += "</body>";
    server.send(200, "text/html", output);
}

void setup() {
  // Start the Serial Monitor
  Serial.begin(115200);
  // Start the DS18B20 sensor
  sensors.begin();

  pinMode(thermostat_relay_pin, OUTPUT);

  WiFi.begin(k_WifiSSID, k_WifiPassword);

  while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
  MDNS.begin(k_HostName);
  server.on("/", HTTP_GET, handle_on_access);
  server.begin();
  MDNS.addService("http", "tcp", 80);

  Serial.printf("Ready! Open http://%s.local in your browser\n", k_HostName);

  auto now = millis();
  next_temp_check = now + k_TimeBetweenTempChecksMillis;

  processTemps();  
  setCompressorState(now, true);
}


void setCompressorState(const long &now, bool force)
{
  if (force || (now >= next_relay_state_change
    && compressor_running != compressor_wants_to_run))
  {
    compressor_running = compressor_wants_to_run;
    next_relay_state_change = last_relay_state_change =  now;

    next_relay_state_change += compressor_running ? k_MinRelayTimeOn : k_MinRelayTimeOff;

    digitalWrite(thermostat_relay_pin, compressor_running);
    Serial.printf("Fridge now on? %s\n", compressor_running ? "on" : "off");
  }
}

void processTemps()
{
  long now = millis();

  if (now >= next_temp_check)
  {
    sensors.requestTemperatures(); 
    current_fridge_temp = sensors.getTempCByIndex(0);
    next_temp_check = now + k_TimeBetweenTempChecksMillis;

    float temp_threshold = desired_fridge_temp;
    bool new_state = compressor_wants_to_run;
    if (new_state && current_fridge_temp < (desired_fridge_temp - k_TempHysteresisCelsius))
    {
      new_state = false;
    }
    else if (!new_state && current_fridge_temp > (desired_fridge_temp + k_TempHysteresisCelsius))
    {
      new_state = true;
    }
    
    if (new_state != compressor_wants_to_run)
    {
      compressor_wants_to_run = new_state;
      Serial.printf("Fridge desired state is now: %s\n", compressor_wants_to_run ? "on" : "off");
    }
  }

  setCompressorState(now);
}

void loop() {
  server.handleClient();
  delay(2);

  processTemps();
}
