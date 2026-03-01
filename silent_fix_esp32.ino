#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Firebase_ESP_Client.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Preferences.h>

#define DEVICE_ID "device_001"
#define RESET_PIN 15
#define CURRENT_PIN 34
#define DHTPIN 4
#define DHTTYPE DHT22
#define ACK_BUTTON_VPIN V10

char ssid[] = "WIFI";
char pass[] = "PASS";
char auth[] = "BLYNK_TOKEN";
#define API_KEY "AIzaSyA7FUgaScWTlOIt5p3ARcluJZzJxuKAGXQ"
#define DATABASE_URL "https://silent-fix-default-rtdb.firebaseio.com/"

DHT dht(DHTPIN, DHTTYPE);
Adafruit_MPU6050 mpu;
Preferences prefs;
FirebaseData fbdo;
FirebaseConfig config;
FirebaseAuth authfb;

float meanC, stdC, meanV, stdV;
bool baselineStored;

float fC=0, fT=0, fV=0;
int anomalyCounter=0;
bool alertSent=false;

int healthScore=100;
bool advisoryAlert=false;
bool acknowledged=false;

float lastTemp=0, lastCurrent=0, lastVibration=0;

BLYNK_WRITE(ACK_BUTTON_VPIN) {
  acknowledged = param.asInt();
}

void setup() {
  Serial.begin(115200);
  pinMode(RESET_PIN, INPUT_PULLUP);

  dht.begin();
  Wire.begin();
  mpu.begin();
  prefs.begin("baseline", false);

  if (digitalRead(RESET_PIN) == LOW) {
    prefs.clear();
    ESP.restart();
  }

  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  Blynk.begin(auth, ssid, pass);

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.begin(&config, &authfb);

  baselineStored = prefs.getBool("stored", false);

  Serial.println("Running startup self-test...");
  Serial.println(baselineStored ? "Baseline OK" : "Baseline missing");

  if (!baselineStored) {
    float c[50], v[50];
    for (int i=0;i<50;i++) {
      c[i] = analogRead(CURRENT_PIN);
      sensors_event_t a,g,t;
      mpu.getEvent(&a,&g,&t);
      v[i] = sqrt(a.acceleration.x*a.acceleration.x +
                  a.acceleration.y*a.acceleration.y +
                  a.acceleration.z*a.acceleration.z);
      delay(2000);
    }
    meanC=0; meanV=0;
    for(int i=0;i<50;i++){meanC+=c[i]; meanV+=v[i];}
    meanC/=50; meanV/=50;

    stdC=0; stdV=0;
    for(int i=0;i<50;i++){
      stdC+=pow(c[i]-meanC,2);
      stdV+=pow(v[i]-meanV,2);
    }
    stdC=sqrt(stdC/50); stdV=sqrt(stdV/50);

    prefs.putFloat("mC",meanC);
    prefs.putFloat("sC",stdC);
    prefs.putFloat("mV",meanV);
    prefs.putFloat("sV",stdV);
    prefs.putBool("stored",true);
  }

  meanC=prefs.getFloat("mC");
  stdC=prefs.getFloat("sC");
  meanV=prefs.getFloat("mV");
  stdV=prefs.getFloat("sV");
}

void loop() {
  Blynk.run();

  float temp = dht.readTemperature();
  if (isnan(temp)) temp = lastTemp;
  else lastTemp = temp;

  float current = analogRead(CURRENT_PIN);
  lastCurrent = current;

  sensors_event_t a,g,t;
  mpu.getEvent(&a,&g,&t);
  float vibration = sqrt(
    a.acceleration.x*a.acceleration.x +
    a.acceleration.y*a.acceleration.y +
    a.acceleration.z*a.acceleration.z
  );
  lastVibration = vibration;

  fC=(fC+current)/2;
  fT=(fT+temp)/2;
  fV=(fV+vibration)/2;

  bool curAn = fC > meanC + 2*stdC;
  bool vibAn = fV > meanV + 2*stdV;

  if (curAn || vibAn) anomalyCounter++;
  else { anomalyCounter=0; alertSent=false; }

  float driftC = fC - meanC;
  float driftV = fV - meanV;
  advisoryAlert = (driftC > stdC && driftV > stdV);

  healthScore = constrain(100 - anomalyCounter*2, 0, 100);

  if (anomalyCounter > 30 && !alertSent) {
    Blynk.logEvent("critical_fault");
    alertSent=true;
  }

  if (acknowledged) {
    anomalyCounter=0;
    alertSent=false;
    acknowledged=false;
  }

  Firebase.RTDB.setFloat(&fbdo,"/devices/device_001/live/current",fC);
  Firebase.RTDB.setFloat(&fbdo,"/devices/device_001/live/temperature",fT);
  Firebase.RTDB.setFloat(&fbdo,"/devices/device_001/live/vibration",fV);
  Firebase.RTDB.setInt(&fbdo,"/devices/device_001/healthScore",healthScore);

  delay(2000);
}