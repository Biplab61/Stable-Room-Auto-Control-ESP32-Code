#include <WiFi.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <Adafruit_SHT4x.h>
#include "EPD_1in9.h"
#include <ArduinoJson.h>

// API Token and Endpoints
String bearerToken = "A7F19D4C2B9E63FA5DB62C8E3E914F";
String updateDataURL = "http://192.168.1.11:3000/api/p/stable-room-device/update-data";
String thresholdURL = "http://192.168.1.11:3000/api/p/stable-room-device/threshold?device_no=D01";
String alertMailURL = "http://192.168.1.11:3000/api/p/stable-room-device/alert-mail";

// Sensor and thresholds
Adafruit_SHT4x sht4 = Adafruit_SHT4x();
sensors_event_t humidity, temp;

float temperature_value;
float humidity_value;
float threshold_temp_min = 0;
float threshold_temp_max = 100;
float threshold_hum_min = 0;
float threshold_hum_max = 100;

int alert_mail_sent = 1;

// GPIO pins
#define AC_PIN 15
#define HEATER_PIN 13

// Display segment arrays
char digit_left[] = {0xbf, 0x00, 0xfd, 0xf5, 0x47, 0xf7, 0xff, 0x21, 0xff, 0xf7, 0x00};
char digit_right[] = {0x1f, 0x1f, 0x17, 0x1f, 0x1f, 0x1d, 0x1d, 0x1f, 0x1f, 0x1f, 0x00};
unsigned char eink_segment[] = {0x00};
char temp_digit[4];
char humidity_digit[3];

void setupGPIO() {
  pinMode(AC_PIN, OUTPUT);
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(AC_PIN, LOW);
  digitalWrite(HEATER_PIN, LOW);
}

void temp_humidity_init() {
  if (!sht4.begin()) {
    Serial.println("Couldn't find SHT4x sensor");
    while (1) delay(10);
  }
  sht4.setPrecision(SHT4X_HIGH_PRECISION);
  sht4.setHeater(SHT4X_NO_HEATER);
}

void wifi_init() {
  WiFiManager wm;
  bool res = wm.autoConnect("StableRoom", "12345678");
  if (!res) Serial.println("WiFi failed to connect");
  Serial.print("Connected to WiFi. IP: ");
  Serial.println(WiFi.localIP());
}

void hum_temp_display() {
  EPD_1in9_lut_GC();
  EPD_1in9_lut_DU_WB();

  temp_digit[0] = char(temperature_value / 100) % 10;
  temp_digit[1] = char(temperature_value / 10) % 10;
  temp_digit[2] = char(temperature_value) % 10;
  temp_digit[3] = int(temperature_value * 10) % 10;

  humidity_digit[0] = char(humidity_value / 10) % 10;
  humidity_digit[1] = char(humidity_value) % 10;
  humidity_digit[2] = int(humidity_value * 10) % 10;

  if (temperature_value < 100) temp_digit[0] = 10;

  eink_segment[0] = digit_right[temp_digit[0]];
  eink_segment[1] = digit_left[temp_digit[1]];
  eink_segment[2] = digit_right[temp_digit[1]];
  eink_segment[3] = digit_left[temp_digit[2]];
  eink_segment[4] = digit_right[temp_digit[2]] | B00100000;
  eink_segment[11] = digit_left[temp_digit[3]];
  eink_segment[12] = digit_right[temp_digit[3]];

  eink_segment[5] = digit_left[humidity_digit[0]];
  eink_segment[6] = digit_right[humidity_digit[0]];
  eink_segment[7] = digit_left[humidity_digit[1]];
  eink_segment[8] = digit_right[humidity_digit[1]] | B00100000;
  eink_segment[9] = digit_left[humidity_digit[2]];
  eink_segment[10] = digit_right[humidity_digit[2]] | B00100000;

  eink_segment[13] = 0x05 | B00001000 | B00010000;

  EPD_1in9_Write_Screen(eink_segment);
  delay(500);
  EPD_1in9_sleep();
}

void notifyDeviceActive() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(updateDataURL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + bearerToken);
    String payload = "{\"device_no\": \"D01\", \"is_device_connected\": true}";
    int response = http.POST(payload);
    Serial.println("Device active status sent.");
    http.end();
  }
}

void fetchThresholds() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(thresholdURL);
    http.addHeader("Authorization", "Bearer " + bearerToken);
    int httpCode = http.GET();

    if (httpCode > 0) {
      String payload = http.getString();
      Serial.println("Threshold Data received.");

      DynamicJsonDocument doc(512);
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.print("JSON parse failed: ");
        Serial.println(error.c_str());
        return;
      }

      threshold_temp_min = doc["min_temperature"] | 0.0;
      threshold_temp_max = doc["max_temperature"] | 100.0;
      threshold_hum_min  = doc["min_humidity"]    | 0.0;
      threshold_hum_max  = doc["max_humidity"]    | 100.0;
      Serial.println("Setting threshold values...");

    } else {
      Serial.println("Failed to fetch threshold data. HTTP code: " + String(httpCode));
    }
    http.end();
  }
}

void sendLiveData(bool ac_on, bool heater_on) {
  if (WiFi.status() == WL_CONNECTED && !isnan(humidity_value) && !isnan(temperature_value)) {
    HTTPClient http;
    http.begin(updateDataURL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + bearerToken);

    String json = "{\"device_no\": \"D01\", \"current_temperature\": " + String(temperature_value, 1) +
                  ", \"current_humidity\": " + String(humidity_value, 1) +
                  ", \"is_ac_on\": " + String(ac_on ? "true" : "false") +
                  ", \"is_heater_on\": " + String(heater_on ? "true" : "false") + "}";

    int responseCode = http.POST(json);
    String responseBody = http.getString();

    Serial.println(">>> Live data send to server.");
    http.end();
    
  } else {
    Serial.println("!!! ERROR: WiFi not connected or sensor data invalid. Skipping DB update.");
  }
}

void sendAlertMail(float temperature_value, float humidity_value) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String fullURL = alertMailURL + "?current_temperature=" + String(temperature_value) +
                     "&current_humidity=" + String(humidity_value) + "&device_no=D01";
    http.begin(fullURL);
    http.addHeader("Authorization", "Bearer " + bearerToken);
    int res = http.POST("");
    Serial.println("Alert mail status: " + String(res));
    http.end();
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  wifi_init();
  temp_humidity_init();
  setupGPIO();
  notifyDeviceActive();
  fetchThresholds();
  GPIOInit();
  EPD_1in9_init();
  EPD_1in9_lut_5S();
  EPD_1in9_Write_Screen(DSPNUM_1in9_off);
  delay(500);
}

void loop() {
  sht4.getEvent(&humidity, &temp);
  temperature_value = temp.temperature;
  humidity_value = humidity.relative_humidity;
 
  Serial.println("----------- SENSOR DATA -----------");
  Serial.print("Temperature: "); 
  Serial.print(temperature_value); 
  Serial.print(" °C, Humidity: ");
  Serial.print(humidity_value); Serial.println(" %");

  hum_temp_display();
  fetchThresholds();

  bool is_ac_on = false;
  bool is_heater_on = false;
  bool tempOutOfRange = false;
  bool humOutOfRange = false;

  // Automatic control
  if (temperature_value >= threshold_temp_max) {
    digitalWrite(AC_PIN, HIGH);
    digitalWrite(HEATER_PIN, LOW);
    is_ac_on = true;
    tempOutOfRange = true;
  } else if (temperature_value <= threshold_temp_min) {
    digitalWrite(AC_PIN, LOW);
    digitalWrite(HEATER_PIN, HIGH);
    is_heater_on = true;
    tempOutOfRange = true;
  } else {
    digitalWrite(AC_PIN, LOW);
    digitalWrite(HEATER_PIN, LOW);
  }


  if (humidity.relative_humidity <= threshold_hum_min || humidity.relative_humidity >= threshold_hum_max) {
    humOutOfRange = true;
  }

  sendLiveData(is_ac_on, is_heater_on);


  if ((tempOutOfRange || humOutOfRange) && (alert_mail_sent == 1)) {
    Serial.println("!!! ALERT: Threshold crossed. Sending email...");
    sendAlertMail(temp.temperature, humidity.relative_humidity);
    alert_mail_sent = 2;
  }

  if (!tempOutOfRange && !humOutOfRange && (alert_mail_sent >= 1)) {
    Serial.println("✅ Temperature and Humidity returned to normal.");
    alert_mail_sent = 1;
  }

  delay(10000); // Delay 10 seconds
}
