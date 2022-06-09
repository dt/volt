#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <TrueRMS.h>

#define LPERIOD 400 // loop period time in us.
#define RMS_WINDOW 125
unsigned long nextLoop;

struct reading
{
  uint16_t t;
  float_t v;
};

const uint16_t limit = 512;
reading readings[limit];
uint16_t cursor = 0;
uint8_t loops = 0;

const char *ssid = SSID;
const char *password = PASSWORD;

Rms readRms;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 SSE</title>
<script src="https://cdn.amcharts.com/lib/4/core.js"></script>
<script src="https://cdn.amcharts.com/lib/4/charts.js"></script>
<script src="https://cdn.amcharts.com/lib/4/themes/animated.js"></script>
<script language="javascript">
  am4core.ready(function() {
    var chart = am4core.create("voltage", am4charts.XYChart);
    chart.events.on("beforedatavalidated", function(ev) {chart.data.sort(function(a, b) {return a.t - b.t;});});
    var xAxis = chart.xAxes.push(new am4charts.ValueAxis());
    var yAxis = chart.yAxes.push(new am4charts.ValueAxis());
    var voltage = chart.series.push(new am4charts.LineSeries());
    chart.dataSource.url = '/readings';
    chart.dataSource.reloadFrequency = 2000;
    voltage.dataFields.valueY = "v";
    voltage.dataFields.valueX = "t";
  });
  </script>
</head>
<body>
  <h2>Volta</h2>
  <div class="content">
    <div class="card">
      <div class="container">
        <div id="voltage" style="width: 2000px; height: 750px;"></div>
      </div>
    </div> 
  </div>
</body>
</html>
)rawliteral";

// Web server running on port 80
AsyncWebServer server(80);

void setupWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED)
  {
    Serial.println("wifi");
    delay(5000);
    ESP.restart();
  }
}

void setupOTA()
{
  ArduinoOTA.setHostname("volta");

  ArduinoOTA
      .onStart([]()
               {
                 String type;
                 if (ArduinoOTA.getCommand() == U_FLASH)
                   type = "sketch";
                 else // U_SPIFFS
                   type = "filesystem";

                 // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
                 Serial.println("Start updating " + type);
               })
      .onEnd([]()
             { Serial.println("\nEnd"); })
      .onProgress([](unsigned int progress, unsigned int total)
                  { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); })
      .onError([](ota_error_t error)
               {
                 Serial.printf("Error[%u]: ", error);
                 if (error == OTA_AUTH_ERROR)
                   Serial.println("Auth Failed");
                 else if (error == OTA_BEGIN_ERROR)
                   Serial.println("Begin Failed");
                 else if (error == OTA_CONNECT_ERROR)
                   Serial.println("Connect Failed");
                 else if (error == OTA_RECEIVE_ERROR)
                   Serial.println("Receive Failed");
                 else if (error == OTA_END_ERROR)
                   Serial.println("End Failed");
               });
  ArduinoOTA.begin();
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Booting");

  setupWifi();
  setupOTA();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send_P(200, "text/html", index_html, NULL); });
  server.on("/readings", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              uint16_t *end = (uint16_t *)(malloc(sizeof(uint16_t) + sizeof(uint16_t)));
              uint16_t *i = end + 1;
              *end = cursor;
              *i = *end + 1;
              AsyncWebServerResponse *response = request->beginChunkedResponse(
                  "application/json",
                  [i, end](uint8_t *buffer, size_t maxLen, size_t index) -> size_t
                  {
                    size_t len = 0;
                    if (maxLen < 64)
                    {
                      for (; len < maxLen; len++)
                      {
                        buffer[len] = ' ';
                      }
                      return len;
                    }

                    maxLen = maxLen - 64; // leave room for final entry plus closing json.

                    if (index == 0)
                    {
                      len += sprintf(((char *)buffer), "[{\"t\":%d,\"v\":%g}", readings[*i].t, readings[*i].v);
                      (*i)++;
                      if (*i == limit)
                      {
                        *i = 0;
                      }
                    }
                    while (*i != *end && len < maxLen)
                    {
                      len += sprintf(((char *)buffer + len), ",{\"t\":%d,\"v\":%g}", readings[*i].t, readings[*i].v);
                      (*i)++;
                      if (*i == limit)
                      {
                        *i = 0;
                      }
                      if (*i == *end)
                      {
                        len += sprintf(((char *)buffer + len), ",{\"t\":%d,\"v\":%g}]\n", readings[*i].t, readings[*i].v);
                      }
                    }
                    if (len == 0)
                    {
                      free(end);
                    }
                    return len;
                  });
              request->send(response);
            });
  server.begin();

  pinMode(32, INPUT);
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  readRms.begin(340.0 * (5.0/3.3), RMS_WINDOW, ADC_12BIT, BLR_ON, CNT_SCAN);
  readRms.start();

  nextLoop = micros() + LPERIOD;
}

void loop()
{
  readRms.update(analogRead(32));
  loops++;
  ArduinoOTA.handle();

  if (loops == 0xEF)
  {
    readRms.publish();
    readings[cursor].t = millis();
    readings[cursor].v = readRms.rmsVal;
    //Serial.printf("%d t: %t, v: %g\n", cursor, readings[cursor].t, readings[cursor].v);
    cursor++;
    if (cursor >= limit)
    {
      cursor = 0;
    }
    loops = 0;
  }

  nextLoop = nextLoop + LPERIOD;
  int16_t next = nextLoop - micros();
  if (next > 1000)
  {
    Serial.printf("loop %d, cursor %d, nextLoop at %d, %d away\n", loops, cursor, nextLoop, next);
  }
  if (next > 20)
  {
    delayMicroseconds(next - 20);
  }
  while (nextLoop >= micros())
    ;
}
