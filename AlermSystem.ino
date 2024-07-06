#include <FastLED.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>
#include <ESPAsyncWebServer.h>
#include <HTTPUpdate.h>
#include <esp_task_wdt.h>

// Constants for LED configurationactivateTfestLedByMacAdrress
#define LED_PIN 25
#define NUM_LEDS 30
#define BRIGHTNESS 50
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

// Global Variables
CRGB leds[NUM_LEDS];
String cityAlermLog = "";
String targetCities[4];
const char *pikudHaorefApiUrl = "https://www.oref.org.il/WarningMessages/alert/alerts.json";
String savedCitiesJson;
String moduleName;
String macAddress = WiFi.macAddress();
String ipAddress = WiFi.localIP().toString();
volatile bool shouldBeDeleted = false;
String version = "0.1.9";


Preferences preferences;
WebServer server(90);
AsyncWebServer serverAsyn(80);

const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 2 * 3600;
const int daylightOffset_sec = 3600;


#define MAX_DEVICES 10
#define RETRY_COUNT 3

IPAddress esp32Addresses[MAX_DEVICES];
int deviceCount = 0;

// Function Declarations
void connectToWifi();
void handleRoot(AsyncWebServerRequest *request);
void handleInfo(AsyncWebServerRequest *request);
void handleTest(AsyncWebServerRequest *request);
void checkAlertCitiesFromPikudHaorefApi();
void ledIsOn();
void PermanentUrl();
void saveCitiesToPreferences();
void loadCitiesFromPreferences();
void configModeCallback(WiFiManager *myWiFiManager);
void handleDisplaySavedCities(AsyncWebServerRequest *request);
void handleSaveCities(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleChangeId(AsyncWebServerRequest *request);
String getFormattedTime();
void handleTriggerLed();
void PingTestWhitMacAddresses();
void sendDataToServerMongo(String log, String route);
void startUpdateTask(void *parameter);
void connectionIndicatorTask(void *pvParameters);
void pingAndTestTask(void *pvParameters);
void sendDataToServerTask(void *pvParameters);
void sendPongBack(const String &macAddress,String testType);
void updateFirmware();
void findESP32AlermDevices();
void getSavedCities(AsyncWebServerRequest *request);
void triggerNewCitiesUpdate();
void fetchAndSaveCities();
void fetchAndSaveCitiesTask(void *parameter);

void setup()
{
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  Serial.begin(115200);
  connectToWifi();

  preferences.begin("alerm", false);
  moduleName = preferences.getString("moduleName", ""); // Use default value if not found

  if (moduleName == "")
  {
    moduleName = String((uint32_t)ESP.getEfuseMac(), HEX);
  }

  preferences.end();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Start the web server

  serverAsyn.on("/", HTTP_GET, [](AsyncWebServerRequest * request)
  {
    handleRoot(request);
  });

  serverAsyn.on("/info", HTTP_GET, [](AsyncWebServerRequest * request)
  {
    handleInfo(request);
  });

  serverAsyn.on("/test", HTTP_GET, [](AsyncWebServerRequest * request)
  {
    handleTest(request);
  });

  serverAsyn.on(
  "/save-new-cities", HTTP_POST, [](AsyncWebServerRequest * request) {}, NULL, handleSaveCities);

  serverAsyn.on("/save-cities", HTTP_GET, [](AsyncWebServerRequest * request)
  {
    handleDisplaySavedCities(request);
  });

  serverAsyn.on("/change-id", HTTP_POST, [](AsyncWebServerRequest * request)
  {
    handleChangeId(request);
  });

  serverAsyn.on("/activateLed", HTTP_POST, [](AsyncWebServerRequest * request)
  {
    ledIsOn();
  });
  serverAsyn.on("/update", HTTP_POST, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", "Attempting firmware update...");
    xTaskCreate(startUpdateTask, "Update Task", 8192, NULL, 1, NULL);
  });

  serverAsyn.on("/resetDevice", HTTP_POST, [](AsyncWebServerRequest * request)
  { delay(1000);
    ESP.restart();
  });

  serverAsyn.on("/get-saved-cities", HTTP_GET, [](AsyncWebServerRequest *request) {
    getSavedCities(request);
  });

  serverAsyn.on("/trigger-new-cities-update", HTTP_GET, [](AsyncWebServerRequest *request){
    triggerNewCitiesUpdate();
    request->send(200, "text/plain", "Triggered new cities update");
  });

  serverAsyn.on("/triger-new-cities", HTTP_GET, [](AsyncWebServerRequest *request){
    xTaskCreatePinnedToCore(
      fetchAndSaveCitiesTask,    // Function to be called
      "fetchAndSaveCitiesTask",  // Name of the task
      8192,                      // Stack size (bytes)
      (void *)new std::string("http://alerm.local/get-saved-cities"), // Parameter to pass
      1,                         // Task priority
      NULL,                      // Task handle
      1                          // Core to run the task on
    );
    request->send(200, "text/plain", "Fetched and saved cities");
  });
  
  serverAsyn.begin();
  Serial.println("HTTP server started");

  // Initialize the watchdog timer with a 20 second timeout
  esp_task_wdt_init(20, true);
  esp_task_wdt_add(nullptr);


  // Load saved cities and set up MDNS
  loadCitiesFromPreferences();
  PermanentUrl();

  sendDataToServerMongo("module is connected " + moduleName, "getModuels");
  sendDataToServerMongo("module is connected " + moduleName, "getLogs");
  String cities = "";
  for (int i = 0; i < sizeof(targetCities) / sizeof(targetCities[0]); i++)
  {
    cities += targetCities[i];
    if (i < sizeof(targetCities) / sizeof(int) - 1)
    {
      cities += ", ";
    }
  }
  sendDataToServerMongo("Target cities for " + moduleName + " is: " + cities, "getLogs");

  xTaskCreate(
    connectionIndicatorTask,
    "ConnectionIndicator",
    8192,
    NULL,
    1,
    NULL
  );

  xTaskCreate(
    pingAndTestTask,
    "PingTestTask",
    8192,
    NULL,
    1,
    NULL
  );


}

void loop()
{

  checkAlertCitiesFromPikudHaorefApi();
  esp_task_wdt_reset();
  delay(1000);
}

// url to choose cities http://alerm.local/
void PermanentUrl()
{
  if (!MDNS.begin("alerm"))
  {
    Serial.println("Error setting up MDNS responder!");
  }
  else
  {
    Serial.println("mDNS responder started");
    MDNS.addService("http", "tcp", 80);
  }
}
void updateNewTargetCities() {
  HTTPClient http;
  http.begin("https://alerm-api-9ededfd9b760.herokuapp.com/api/getSavedCities");
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.println("Failed to parse JSON");
      return;
    }

    String macAddressFromServer = doc["macAddress"].as<String>();
    JsonArray citiesArray = doc["cities"].as<JsonArray>();

    Serial.print("The new cities for : " + macAddressFromServer + " are: ");
    for (JsonVariant city : citiesArray) {
      Serial.print(city.as<String>() + " ");
    }
    Serial.println();

    if (macAddressFromServer == macAddress) {
      DynamicJsonDocument citiesDoc(2048);
      citiesDoc["cities"] = citiesArray;

      String citiesJson;
      serializeJson(citiesDoc, citiesJson);


      size_t len = citiesJson.length();
      uint8_t *data = (uint8_t *)malloc(len + 1);
      if (data != nullptr) {
        memcpy(data, citiesJson.c_str(), len + 1);

        AsyncWebServerRequest *request = nullptr;
        handleSaveCities(request, data, len, 0, len);

        free(data);
      } else {
        Serial.println("Failed to allocate memory");
      }
    } else {
      Serial.println("MAC address does not match");
    }
  } else {
    Serial.print("HTTP request failed, error: ");
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}

void handleSaveCities(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  static String bodyData;

  if (index == 0) {
    Serial.println("Receiving body data...");
    bodyData = "";
  }

  for (size_t i = 0; i < len; i++) {
    bodyData += (char)data[i];
  }

  if (index + len == total) {
    Serial.println("Body received completely.");
    Serial.println(bodyData);

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, bodyData);
    if (error) {
      Serial.println("Parsing JSON failed!");
      if (request != nullptr) {
        request->send(400, "text/plain", "Invalid JSON");
      }
      return;
    }

    JsonArray cities = doc["cities"].as<JsonArray>();

    for (int i = 0; i < 4; i++) {
      targetCities[i] = "";
    }

    size_t i = 0;
    for (JsonVariant city : cities) {
      if (i < 4) {
        targetCities[i] = city.as<String>();
        Serial.println("City: " + targetCities[i]);
        i++;
      }
    }

    saveCitiesToPreferences();

    if (request != nullptr) {
      request->send(200, "application/json", "{\"message\":\"Cities received and processed.\"}");
    }
    
    String logMessage = "Target city change To: ";
    for (int i = 0; i < 4; i++) {
      logMessage += targetCities[i];
      if (i < 3) {
        logMessage += ", ";
      }
    }
    logMessage += " for module: " + moduleName + " ";
    
    Serial.println("Sending log to server: " + logMessage);
//    sendDataToServerMongo(logMessage, "getLogs");
//    sendDataToServerMongo(logMessage, "getModuels");
    Serial.println("Log sent to server.");
  }
}



void saveCitiesToPreferences()
{
  DynamicJsonDocument doc(4096);
  JsonArray array = doc.to<JsonArray>();

  for (int i = 0; i < sizeof(targetCities) / sizeof(targetCities[0]); i++)
  {
    if (targetCities[i] != "")
    {
      array.add(targetCities[i]);
    }
  }

  // Convert JSON array to string
  String jsonString;
  serializeJson(array, jsonString);

  preferences.begin("my-app", false);
  preferences.putString("savedCities", jsonString);
  preferences.end();
}

void loadCitiesFromPreferences()
{
  size_t maxCities = sizeof(targetCities) / sizeof(targetCities[0]);

  preferences.begin("my-app", true);
  String jsonString = preferences.getString("savedCities", "");
  preferences.end();

  DynamicJsonDocument doc(4096);
  deserializeJson(doc, jsonString);
  JsonArray array = doc.as<JsonArray>();

  size_t cityIndex = 0;
  for (JsonVariant city : array)
  {
    if (cityIndex < maxCities)
    {
      targetCities[cityIndex++] = city.as<String>();
    }
  }
  for (int j = 0; j < 1; j++)
  {
    String logMessage = "Target cities: ";

    // Constructing the list of target cities
    for (int i = 0; i < sizeof(targetCities) / sizeof(targetCities[0]); i++)
    {
      logMessage += targetCities[i];
      if (i < sizeof(targetCities) / sizeof(targetCities[0]) - 1)
      {
        logMessage += ", ";
      }
    }

    logMessage += " for module: " + moduleName + " ";


    sendDataToServerMongo(logMessage, "getLogs");
  }
}

void configModeCallback(WiFiManager *myWiFiManager)
{
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void connectToWifi()
{
  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  String ssid = "Alerm System";

  Serial.println("Connecting to: " + ssid);

  if (!wifiManager.autoConnect(ssid.c_str()))
  {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
  }
  else
  {
    Serial.println("Success: Connected to WiFi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  }
}

void handleInfo(AsyncWebServerRequest *request)
{
  String wifiName = WiFi.SSID();
  uint32_t chipId = ESP.getEfuseMac();
  IPAddress ip = WiFi.localIP();
  String macAddress = WiFi.macAddress();
  String htmlContent = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">  
     <link
      rel="icon"
      href="https://alerm-api-9ededfd9b760.herokuapp.com/api/favicon"
    />
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Info Page</title>
    <style>
    body {
        font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
        margin: 0;
        padding: 20px;
        background-color: #f4f4f4;
        color: #333;
    }
    h1 {
        color: #4CAF50;
        margin-bottom: 20px;
    }
    p {
        font-size: 18px;
        line-height: 1.6;
        color: #666;
    }
    div {
        background: white;
        padding: 20px;
        border-radius: 5px;
        box-shadow: 0 5px 15px rgba(0,0,0,0.1);
        margin-bottom: 20px;
    }
    form {
        background: white;
        padding: 20px;
        border-radius: 5px;
        box-shadow: 0 5px 15px rgba(0, 0, 0, 0.1);
        max-width: 500px;
        margin: 20px auto;
//        direction:rtl;
    }
    label {
        font-size: 18px;
        color: #555;
    }
    input[type="text"],
    input[type="submit"] {
        width: calc(100% - 22px);
        padding: 10px;
        margin: 8px 0;
        display: inline-block;
        border: 1px solid #ccc;
        border-radius: 4px;
        box-sizing: border-box;
    }
    input[type="submit"] {
        width: 100%;
        background-color: #007bff;
        color: white;
        cursor: pointer;
    }
    input[type="submit"]:hover {
        background-color: #0056b3;
    }
    nav ul {
        list-style-type: none;
        display: flex;
        flex-direction: row;
        justify-content: space-around;
        padding-right: 0px;
        padding-left: 0px;
    }
    nav li {
        background-color: #fff;
        margin: 10px 0;
        padding: 10px;
        border-radius: 5px;
        box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    nav a {
        color: black;
        text-decoration: none;
        font-weight: bold;
    }
    .Change-Name{
      direction:ltr;`
    }
    </style>
</head>
<body>
<nav>
    <ul>
      <li><a href="/">Home</a></li>
      <li><a href="/save-cities">Cities</a></li>
      <li><a href="/test">Test</a></li>
      <li><a href="/info">Info</a></li>
    </ul>
</nav>
<div>
    <h2>Module Details:</h2>
    <p>WiFi Name: )" + wifiName +
                       R"(</p>
    <p>Module Name: )" +
                       moduleName + R"(</p>
    <p>IP Address: )" + ip.toString() +
                       R"(</p>
    <p>MAC Address: )" +
                       macAddress + R"(</p>

    <p>VERSION: )" +
                       version + R"(</p>
</div>
<form id="changeID" class = "Change-Name:" action="/change-id" method="POST">
    <label for="newId">Change Name:</label>
    <input type="text" id="newId" name="newId" placeholder="New name">
    <input type="submit" value="Change Name">
</form>
</body>
</html>
)";

  request->send(200, "text/html", htmlContent);
}

void handleTest(AsyncWebServerRequest *request)
{
  String htmlContent = R"(
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
         <link
      rel="icon"
      href="https://alerm-api-9ededfd9b760.herokuapp.com/api/favicon"
    />
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>Test Page</title>
        <style>
            body {
                font-family: Arial, sans-serif;
                margin: 0;
                padding: 20px;
                background-color: #f4f4f4;
                color: #333;
            }
            button {
                display: block;
                width: 200px;
                padding: 10px;
                margin: 20px auto;
                background-color: #007bff;
                color: white;
                border: none;
                border-radius: 5px;
                cursor: pointer;
                font-size: 16px;
                transition: background-color 0.2s;
            }
            button:hover {
                background-color: #0056b3;
            }
               nav ul {
        list-style-type: none;
        display: flex;
        flex-direction: row;
        justify-content: space-around;
        padding-right: 0px;
        padding-left: 0px;
    }
    nav li {
        background-color: #fff;
        margin: 10px 0;
        padding: 10px;
        border-radius: 5px;
        box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    nav a {
        color: black;
        text-decoration: none;
        font-weight: bold;
    }
        </style>
    </head>
    <body>
 <nav>
        <ul>
          <li><a href="/" ${currentRoute === "/" ? 'style="font-weight:bold;"' : ""}>Home</a></li>
          <li><a href="/save-cities" ${currentRoute === "/cities" ? 'style="font-weight:bold;"' : ""}>Cities</a></li>
          <li><a href="/test" ${currentRoute === "/test" ? 'style="font-weight:bold;"' : ""}>Test</a></li>
          <li><a href="/info" ${currentRoute === "/Info" ? 'style="font-weight:bold;"' : ""}>Info</a></li>

        </ul>
    </nav>
    
        <button id="activateLedButton">Test LED</button>
        <button id="resetButton">Reset Device</button>
        <button id="updateFirmware">Update Device</button>

        <script>
            document.getElementById('activateLedButton').addEventListener('click', function() {
                fetch('/activateLed', { method: 'POST' })
                    .then(response => response.text())
                    .then(data => {
                        console.log(data);
                        // Additional logic after LEDs are activated
                    })
                    .catch(error => {
                        console.error('Error:', error);
                    });
            });
            document.getElementById('resetButton').addEventListener('click', function() {
                fetch('/resetDevice', { method: 'POST' })
                    .then(response => {
                        if(response.ok) {
                            console.log('Device is resetting...');
                        } else {
                            throw new Error('Failed to reset device');
                        }
                    })
                    .catch(error => {
                        console.error('Error:', error);
                    });
            });

                        document.getElementById('updateFirmware').addEventListener('click', function() {
                fetch('/update', { method: 'POST' })
                    .then(response => {
                        if(response.ok) {
                            console.log('Device is resetting...');
                        } else {
                            throw new Error('Failed to reset device');
                        }
                    })
                    .catch(error => {
                        console.error('Error:', error);
                    });
            });
        </script>
    </body>
    </html>
  )";

  request->send(200, "text/html", htmlContent);
}

void handleRoot(AsyncWebServerRequest *request)
{

  String htmlContent = R"(
<!DOCTYPE html>
<html lang='en'>
<head>
    <meta charset='UTF-8'>        
     <link
      rel="icon"
      href="https://alerm-api-9ededfd9b760.herokuapp.com/api/favicon"
    />
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Cities</title>
 <style>
        body {
            font-family: 'Arial', sans-serif;
            background-color: #f4f4f4;
            margin: 0;
            padding: 20px;
            color: #333;
            direction:rtl;
        }
        h1,h3 ,h2{
            text-align: center;
            color: #444;
        }
        
        #changeID{
            text-align: center;
          }
        
        #filterInput {
            display: block;
            margin: 20px auto;
            padding: 10px;
            width: 90%;
            max-width: 500px;
            border: 1px solid #ddd;
            border-radius: 5px;
        }
        #cityForm {
            background: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0, 0, 0, 0.1);
        }
        .city-label {
            display: block;
            margin: 10px 0;
            padding-bottom: 10px;
            border-bottom: 1px solid #ddd; 
            cursor: pointer;
        }
nav ul {
        list-style-type: none; 
        display: flex; 
        flex-direction:row-reverse;
        justify-content:space-around; 
         padding-right: 0px;  
        padding-left: 0px;        
            
    }
    nav li {
        background-color: #fff;
        margin: 10px 0;
        padding: 10px;
        border-radius: 5px;
        box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    nav a {
        color: black; 
        text-decoration: none;
        font-weight: bold;
    }


        
        .city-label:last-child {
            border-bottom: none;
        }
        input[type="submit"] {
            display: block;
            background-color: #007bff;
            color: white;
            padding: 10px 20px;
            border: none;
            border-radius: 5px;
            margin: 20px auto;
            cursor: pointer;
        }
        input[type="submit"]:hover {
            background-color: #0056b3;
        }
               @media (max-width: 768px) {
            button {
                width: auto;
            }
        }
    </style>
</head>
<body>
  <nav>
        <ul>
          <li><a href="/" ${currentRoute === "/" ? 'style="font-weight:bold;"' : ""}>Home</a></li>
          <li><a href="/save-cities" ${currentRoute === "/cities" ? 'style="font-weight:bold;"' : ""}>Cities</a></li>
          <li><a href="/test" ${currentRoute === "/test" ? 'style="font-weight:bold;"' : ""}>Test</a></li>
          <li><a href="/info" ${currentRoute === "/Info" ? 'style="font-weight:bold;"' : ""}>Info</a></li>

        </ul>
    </nav>
    <h2>שם:  )" + moduleName +
                       R"(</h2>
)";

  htmlContent += R"(
    <input type='text' id='filterInput' placeholder='חפש איזורים...'>
    <form id='cityForm'>
        <div id='cityList'></div>
        <input type='submit' value='שמור אזורים'>
    </form>
   
           <script>
     document.getElementById('cityForm').onsubmit = function(event) {
        event.preventDefault();
        var checkedBoxes = document.querySelectorAll('input[name=city]:checked');
        var targetCities = Array.from(checkedBoxes).map(box => box.value);
        
        fetch('/save-new-cities', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ cities: targetCities })
        }).then(function(response) {
            if (response.ok) {
                return fetch('/trigger-new-cities-update', {
                    method: 'GET'
                });
            } else {
                throw new Error('Server responded with status ' + response.status);
            }
        }).then(function(response) {
            if (response.ok) {
                window.location.href = '/save-cities';
            } else {
                throw new Error('Server responded with status ' + response.status);
            }
        }).catch(function(error) {
            console.error('Error:', error);
        });
    };


        document.getElementById('filterInput').oninput = function() {
            var filter = this.value.toUpperCase();
            var labels = document.querySelectorAll('.city-label');
            labels.forEach(label => {
                var text = label.textContent || label.innerText;
                label.style.display = text.toUpperCase().includes(filter) ? '' : 'none';
            });
        };

        fetch('https://alerm-api-9ededfd9b760.herokuapp.com/citiesjson')
        .then(response => response.json())
        .then(cities => {
            var cityListContainer = document.getElementById('cityList');
            cities.forEach(city => {
                var label = document.createElement('label');
                label.classList.add('city-label');
                var checkbox = document.createElement('input');
                checkbox.type = 'checkbox';
                checkbox.name = 'city';
                checkbox.value = city;
                label.appendChild(checkbox);
                label.appendChild(document.createTextNode(` ${city}`));
                cityListContainer.appendChild(label);
            });
        })
        .catch(error => {
            console.error('Error fetching the cities:', error);
        });
    </script>
    
    </body>
</html>
)";


  request->send(200, "text/html", htmlContent);
}

void handleDisplaySavedCities(AsyncWebServerRequest *request)
{
  // Parse the JSON
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, savedCitiesJson);

  JsonArray cities = doc["cities"].as<JsonArray>();

  String responseHtml = R"(
<!DOCTYPE html>
<html lang='en'>
<head>
    <meta charset='UTF-8'>
     <link
      rel="icon"
      href="https://alerm-api-9ededfd9b760.herokuapp.com/api/favicon"
    />
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>אזורים שנבחרו</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background-color: #e9ecef;
            color: #495057;
            margin: 0;
            padding: 20px;
            direction:rtl
        }
        h1 {
            text-align: center;
            color: #212529;
        }
        ul {
            list-style-type: none;
            padding: 0;
        }
        li {
            background-color: white;
            margin: 10px 0;
            padding: 15px;
            border-radius: 8px;
            box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);
            transition: transform 0.2s;
        }
        li:hover {
            transform: translateY(-2px);
        }

nav ul {
        list-style-type: none; 
        display: flex; 
        flex-direction:row-reverse;
        justify-content:space-around; 
        padding-right: 0px;
        padding-right: 0px;        
    }
    nav li {
        background-color: #fff;
        margin: 10px 0;
        padding: 10px;
        border-radius: 5px;
        box-shadow: 0 2px 4px rgba(0,0,0,0.1);
    }
    nav a {
        color: black; 
        text-decoration: none;
        font-weight: bold;
    }

        button {
            display: block;
            width: 200px;
            padding: 10px;
            margin: 20px auto;
            background-color: #007bff;
            color: white;
            border: none;
            border-radius: 5px;
            cursor: pointer;
            font-size: 16px;
            transition: background-color 0.2s;
        }
        button:hover {
            background-color: #0056b3;
        }
        @media (max-width: 768px) {
            button {
                width: auto;
            }
        }
    </style>
</head>
<body>
<nav>
        <ul>
          <li><a href="/" ${currentRoute === "/" ? 'style="font-weight:bold;"' : ""}>Home</a></li>
          <li><a href="/save-cities" ${currentRoute === "/cities" ? 'style="font-weight:bold;"' : ""}>Cities</a></li>
          <li><a href="/test" ${currentRoute === "/test" ? 'style="font-weight:bold;"' : ""}>Test</a></li>
          <li><a href="/info" ${currentRoute === "/Info" ? 'style="font-weight:bold;"' : ""}>Info</a></li>

        </ul>
    </nav>
    <h1>אזורים שנבחרו</h1>
    <ul>
)";

  for (int i = 0; i < sizeof(targetCities) / sizeof(targetCities[0]); i++)
  {
    if (targetCities[i] != "")
    {
      responseHtml += "<li>" + targetCities[i] + "</li>";
    }
  }

  responseHtml += R"(
    </ul>
    <a href='/'><button>חזור</button></a>
</body>
</html>
)";

  request->send(200, "text/html", responseHtml);
}

void handleChangeId(AsyncWebServerRequest *request)
{
  if (request->hasParam("newId", true))
  { 
    AsyncWebParameter *p = request->getParam("newId", true);
    moduleName = p->value(); // Update the moduleName with the new value
    Serial.println("ID changed to: " + moduleName);
    sendDataToServerMongo("module name is change to: " + moduleName, "getModuels");
    sendDataToServerMongo("module name is change to: " + moduleName, "getLogs");

    
    preferences.begin("alerm", false);
    preferences.putString("moduleName", moduleName);
    preferences.end();

    request->redirect("/");
  }
  else if (request->hasParam("newId", false))
  { 
    AsyncWebParameter *p = request->getParam("newId", false);
    moduleName = p->value(); 
    Serial.println("ID changed to: " + moduleName);

    preferences.begin("alerm", false);
    preferences.putString("moduleName", moduleName);
    preferences.end();

    request->redirect("/");
  }
  else
  {
    request->send(400, "text/plain", "Bad Request: newId not provided");
  }
}

void checkAlertCitiesFromPikudHaorefApi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    http.begin(client, pikudHaorefApiUrl);
    http.addHeader("Referer", "https://www.oref.org.il/11226-he/pakar.aspx");
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_13_6) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/75.0.3770.100 Safari/537.36");

    int httpCode = http.GET();
    //    Serial.print("HTTP Status Code: ");
    //    Serial.println(httpCode);

    if (httpCode == 200)
    {
      String payload = http.getString();
      //      Serial.println("Server response:");
      Serial.println(payload);

      if (payload.startsWith("\xEF\xBB\xBF"))
      {
        payload = payload.substring(3); // Remove BOM
      }
      for (int i = 0; i < sizeof(targetCities) / sizeof(targetCities[0]); i++)
      {
        //        Serial.println("Target city: " + targetCities[i]);
      }

      DynamicJsonDocument doc(4096);
      DeserializationError error = deserializeJson(doc, payload);

      if (!error)
      {

        JsonArray dataArray = doc["data"].as<JsonArray>();
        String jsonArrayAsString;
        serializeJson(dataArray, jsonArrayAsString);
        Serial.println("Data array: " + jsonArrayAsString);

        bool alertDetected = false;

        for (JsonVariant cityValue : dataArray)
        {
          for (String city : targetCities)
          {
            Serial.println(city + " compare with " + cityValue.as<String>());

            if (cityValue.as<String>() == city)
            {
              alertDetected = true;
              Serial.println("Alert for: " + city);
              cityAlermLog = city;
              break;
            }
          }
          if (alertDetected)
          {
            break;
          }
        }

        if (alertDetected) {
            Serial.println("Triggering alarm...");
            String *logMessage = new String("alert active at " + moduleName + " in city:" + cityAlermLog);
        
            xTaskCreate(sendDataToServerTask, "SendDataServerTask", 8192, (void*)logMessage, 1, NULL);
        
            ledIsOn();
        }
        else
        {
          Serial.println("No alert for target cities.");
        }
      }
      else
      {
        //        Serial.print("deserializeJson() failed: ");
        //        Serial.println(error.c_str());
      }
    }
    else
    {
      Serial.print("HTTP request failed, error: ");
      Serial.println(http.errorToString(httpCode));
    }
    leds[0] = CRGB::Green;
    FastLED.show();
    http.end();
  }
  else
  {
    leds[0] = CRGB::Red;
    FastLED.show();
    //    Serial.println("Disconnected from WiFi. Trying to reconnect...");
  }
  delay(1000);
}



String getFormattedTime()
{
  struct tm timeinfo;
  int retry = 0;
  const int retry_count = 3;
  while (!getLocalTime(&timeinfo) && ++retry < retry_count)
  {
    Serial.println("Failed to obtain time, retrying...");
    delay(500); // Wait half a second before retrying
  }

  if (retry >= retry_count)
  {
    Serial.println("Failed to obtain time after several attempts");
    return "";
  }

  char timeStringBuff[80]; // 80 chars should be enough
  strftime(timeStringBuff, sizeof(timeStringBuff), " %Y-%m-%dT%H:%M:%SZ ", &timeinfo);
  Serial.println(timeStringBuff); // Print the time to the Serial monitor
  return String(timeStringBuff);
}

void ledIsOn()
{
  int blinkDurationInSeconds = 10;
  int blinkSpeedInMillis = 100;

  int numberOfBlinks = (blinkDurationInSeconds * 1000) / (blinkSpeedInMillis * 2);

  for (int j = 0; j < numberOfBlinks; j++)
  {
    for (int i = 0; i < NUM_LEDS; i++)
    {
      leds[i] = CRGB::Red;
    }
    FastLED.show();
    delay(blinkSpeedInMillis);

    for (int i = 0; i < NUM_LEDS; i++)
    {
      leds[i] = CRGB::Black;
    }
    FastLED.show();
    delay(blinkSpeedInMillis);
  }
}

void PingTestWhitMacAddresses()
{

  HTTPClient http;
  http.begin("https://alerm-api-9ededfd9b760.herokuapp.com/api/pingModule");
  int httpResponseCode = http.GET();

  if (httpResponseCode == HTTP_CODE_OK)
  {
    String response = http.getString();
    StaticJsonDocument<256> doc;
    deserializeJson(doc, response);

    String macAddress = WiFi.macAddress();
    if (doc["macAddress"].as<String>() == macAddress && doc["testType"].as<String>() == "PingTest")
    {
      Serial.println("MAC address matches. Sending pong back to server.");
      sendPongBack(macAddress,doc["testType"].as<String>());
    }
    else if (doc["macAddress"].as<String>() == macAddress && doc["testType"].as<String>() == "LedTest")
    {
      Serial.println("Test led sucsses");
      sendPongBack(macAddress,doc["testType"].as<String>());
      ledIsOn();
    }
    else if (doc["macAddress"].as<String>() == macAddress && doc["testType"].as<String>() == "Reset")
    {
      Serial.println("Reset sucsses");
      sendPongBack(macAddress,doc["testType"].as<String>());
      ESP.restart();
    }
        else if (doc["macAddress"].as<String>() == macAddress && doc["testType"].as<String>() == "Update")
    {
      Serial.println("Update sucsses");
      sendPongBack(macAddress,doc["testType"].as<String>());
      updateFirmware();
        }else if(doc["macAddress"].as<String>() == macAddress && doc["testType"].as<String>() == "changeCities"){
          Serial.println("change Cities sucsses");
          sendPongBack(macAddress,doc["testType"].as<String>());
          updateNewTargetCities();
          

          }
    else
    {
      Serial.println("MAC address does not match.");
    }
  }

  http.end();
}

void sendPongBack(const String &macAddress,String testType)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient httpPong;
  httpPong.begin(client, "https://alerm-api-9ededfd9b760.herokuapp.com/api/pongReceivedFromModule");
  httpPong.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> pongDoc;
  pongDoc["message"] = "sucsses";
  pongDoc["macAddress"] = macAddress;
  pongDoc["testType"] = testType;

  String pongPayload;
  serializeJson(pongDoc, pongPayload);

  int pongResponseCode = httpPong.POST(pongPayload);

  if (pongResponseCode == HTTP_CODE_OK)
  {
    Serial.println("Pong sent successfully.");
  }
  else
  {
    Serial.print("Error sending pong: ");
    Serial.println(httpPong.errorToString(pongResponseCode));
    Serial.println(pongResponseCode);
  }

  httpPong.end();
}

void conectionIndecator(const String &macAddress)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient httpPong;
  String url = "https://alerm-api-9ededfd9b760.herokuapp.com/api/moduleIsConnectIndicator/" + macAddress;
  httpPong.begin(url.c_str());
  httpPong.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> pongDoc;
  pongDoc["message"] = true;
  pongDoc["macAddress"] = macAddress;

  String pongPayload;
  serializeJson(pongDoc, pongPayload);
  //    Serial.print(pongPayload);

  int pongResponseCode = httpPong.POST(pongPayload);

  if (pongResponseCode == HTTP_CODE_OK)
  {
    Serial.println("connected sent successfully.");
  }
  else
  {
    Serial.print("Error sending pong: ");
    Serial.println(httpPong.errorToString(pongResponseCode));
    Serial.println(pongResponseCode);
  }

  httpPong.end();
}
void sendDataToServerMongo(String log, String route) {
  IPAddress ip = WiFi.localIP();
  String sensorData = "";
  String targetCitiesJson = "[";

  // Convert targetCities array to JSON array string
  for (int i = 0; i < 4; i++) {
    targetCitiesJson += "\"" + targetCities[i] + "\"";
    if (i < 3) {
      targetCitiesJson += ",";
    }
  }
  targetCitiesJson += "]";

  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); // Use this if you do not want to validate the server's SSL certificate

    HTTPClient http;
    String url = "https://alerm-api-9ededfd9b760.herokuapp.com/api/" + route;
    Serial.println("URL: " + url);

    // Begin HTTP connection
    Serial.println("Beginning connection to server...");
    if (!client.connect("alerm-api-9ededfd9b760.herokuapp.com", 443)) {
      Serial.println("Connection to server failed");
      return;
    }
    Serial.println("Connection to server successful");

    if (!http.begin(client, url)) {
      Serial.println("HTTP begin failed");
      return;
    }

    http.addHeader("Content-Type", "application/json");

    if (route == "getModuels") {
      sensorData = "{\"macAddress\": \"" + macAddress + "\", \"timestamp\": \"" + getFormattedTime() + "\", \"moduleName\": \"" + moduleName + "\", \"log\": \"" + log + "\", \"ipAddress\": \"" + ip.toString() + "\", \"version\": \"" + version + "\", \"targetCities\": " + targetCitiesJson + "}";
    } else if (route == "getLogs") {
      sensorData = "{\"macAddress\": \"" + macAddress + "\", \"timestamp\": \"" + getFormattedTime() + "\", \"moduleName\": \"" + moduleName + "\", \"log\": \"" + log + "\", \"targetCities\": " + targetCitiesJson + "}";
    }

    Serial.println("Sending data: " + sensorData);

    int httpResponseCode = http.POST(sensorData);

    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.println("HTTP Response code: " + String(httpResponseCode));
      Serial.println("Server response: " + response);
    } else {
      Serial.print("Error on sending POST: ");
      Serial.println(httpResponseCode);
      Serial.println(http.errorToString(httpResponseCode).c_str());
    }

    http.end(); // End HTTP connection
  } else {
    Serial.println("WiFi not connected");
  }
}



void connectionIndicatorTask(void *pvParameters)
{
  while (1)
  {
    conectionIndecator(macAddress);

    vTaskDelay(pdMS_TO_TICKS(60000));

    if (shouldBeDeleted)
    {
      break; 
    }
  }

  vTaskDelete(NULL);
}

void pingAndTestTask(void *pvParameters)
{
  while (1)
  {
    PingTestWhitMacAddresses();

   

    if (shouldBeDeleted)
    {
      break; 
    }
  }

  vTaskDelete(NULL);
}


void updateFirmware() {
  Serial.println("Starting OTA update...");
  HTTPClient httpClient;
  httpClient.begin("https://alerm-api-9ededfd9b760.herokuapp.com/api/update");
  httpClient.setTimeout(300000); 


  t_httpUpdate_return ret = httpUpdate.update(httpClient, ""); 

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("HTTP_UPDATE_NO_UPDATES");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("HTTP_UPDATE_OK - Rebooting...");
      ESP.restart();
      break;
  }
}
void startUpdateTask(void *parameter) {
  updateFirmware();
  vTaskDelete(NULL); 
}
void sendDataToServerTask(void *pvParameters) {
  String *logMessage = (String *)pvParameters;
  
(*logMessage, "getLogs");
  delete logMessage; // Free the allocated memory
  vTaskDelete(NULL); // Terminate the task
}

void findESP32AlermDevices() {
  deviceCount = 0;

  // Start MDNS query for services
  int n = MDNS.queryService("http", "tcp");

  if (n == 0) {
    Serial.println("No alerm services found");
  } else {
    Serial.print(n);
    Serial.println(" alerm services found");
    for (int i = 0; i < n; ++i) {
      String hostname = MDNS.hostname(i);
      // בדיקה אם ה-hostname מכיל את המחרוזת "esp32-alerm"
      if (hostname.startsWith("alerm") && deviceCount < MAX_DEVICES) {
        esp32Addresses[deviceCount] = MDNS.IP(i);
        deviceCount++;
        Serial.print("ESP32 Alerm service: ");
        Serial.print(hostname);
        Serial.print(" at ");
        Serial.println(MDNS.IP(i));
      } else {
        Serial.print("Non-ESP32 service: ");
        Serial.print(hostname);
        Serial.print(" at ");
        Serial.println(MDNS.IP(i));
      }
    }
  }
}

void getSavedCities(AsyncWebServerRequest *request) {
  StaticJsonDocument<200> jsonDoc;
  JsonArray jsonArray = jsonDoc.to<JsonArray>();

  for (size_t i = 0; i < sizeof(targetCities) / sizeof(targetCities[0]); i++) {
    jsonArray.add(targetCities[i]);
  }

  String jsonString;
  serializeJson(jsonDoc, jsonString);

  request->send(200, "application/json", jsonString);
}

void triggerNewCitiesUpdate() {

  findESP32AlermDevices();
  Serial.println("Starting triggerNewCitiesUpdate...");
  for (int i = 0; i < deviceCount; i++) {
    String url = "http://" + esp32Addresses[i].toString() + "/triger-new-cities";
    Serial.print("Sending request to: ");
    Serial.println(url);
    
    std::string *urlParam = new std::string(url.c_str()); // Using std::string to ensure the URL persists
    xTaskCreatePinnedToCore(
      fetchAndSaveCitiesTask,    // Function to be called
      "fetchAndSaveCitiesTask",  // Name of the task
      8192,                      // Stack size (bytes)
      (void *)urlParam,          // Parameter to pass
      1,                         // Task priority
      NULL,                      // Task handle
      1                          // Core to run the task on
    );
    
    delay(1000);
  }
  Serial.println("Finished triggerNewCitiesUpdate.");
}

void fetchAndSaveCitiesTask(void *parameter) {
  std::string *url = (std::string *)parameter;
  Serial.println("Starting fetchAndSaveCities for URL: " + String(url->c_str()));
  
  bool success = false;
  int retries = 0;

  while (!success && retries < RETRY_COUNT) {
    HTTPClient http;
    http.setTimeout(10000); // Set timeout to 10 seconds
    http.begin(url->c_str());
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println("Payload received: " + payload);

      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.println("Failed to parse JSON");
      } else {
        JsonArray citiesArray = doc.as<JsonArray>();

        Serial.print("The new cities are: ");
        for (JsonVariant city : citiesArray) {
          Serial.print(city.as<String>() + " ");
        }
        Serial.println();

        // Create a new JSON document with the cities array
        DynamicJsonDocument citiesDoc(2048);
        citiesDoc["cities"] = citiesArray;

        String citiesJson;
        serializeJson(citiesDoc, citiesJson);

        size_t len = citiesJson.length();
        uint8_t *data = (uint8_t *)malloc(len + 1);
        if (data != nullptr) {
          memcpy(data, citiesJson.c_str(), len + 1);

          AsyncWebServerRequest *request = nullptr;
          Serial.println("Calling handleSaveCities...");
          handleSaveCities(request, data, len, 0, len);

          free(data);
          success = true;
        } else {
          Serial.println("Failed to allocate memory");
        }
      }
    } else {
      Serial.print("HTTP request failed, error: ");
      Serial.println(http.errorToString(httpCode));
    }

    http.end();

    if (!success) {
      retries++;
      Serial.println("Retrying... (" + String(retries) + "/" + String(RETRY_COUNT) + ")");
      delay(2000); // Wait before retrying
    }
  }
  
  Serial.println("Finished fetchAndSaveCities for URL: " + String(url->c_str()));
  delete url;
  vTaskDelete(NULL);
}
