#include <ESP8266WiFi.h>
#include <WiFiManager.h> // Include the WiFiManager library
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>  // Include ArduinoJson library
#include <FS.h> // Include the SPIFFS library
#include <EEPROM.h>

ESP8266WebServer server(80);
#define EEPROM_SIZE 512
#define CITY_ADDRESS 0

// Global variable
String savedCitiesJson;
const int buzzerPin = 2;
const int wifiIndicatorPin = 5;
const int alertIndicatorPin = 16;
const int serverConnection = 13;
const char *apiUrlAlert = "https://alerm-script.onrender.com/get-alerts";
void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  // Initialize SPIFFS
  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }

  // List all files in SPIFFS (for debugging purposes)
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    String fileName = dir.fileName();
    size_t fileSize = dir.fileSize();
    Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), String(fileSize).c_str());
  }

  // Initialize other components
  pinInit();
  wifiConfigAndMDNS();
  loadCitiesFromEEPROM();

  Serial.println("Connected to WiFi");
  digitalWrite(wifiIndicatorPin, HIGH);

  
  // Set up the web server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save-cities", HTTP_POST, handleSaveCities);
  server.on("/save-cities", HTTP_GET, handleDisplaySavedCities);
  server.begin(); // Start the ESP8266 web server

}
void loop()
{
  connectToAlertServer();
  server.handleClient(); // Handle client requests
  MDNS.update();
  Serial.println("savedCitiesJsonLOOP "+savedCitiesJson);




}


void pinInit()
{
  pinMode(buzzerPin, OUTPUT);
  pinMode(wifiIndicatorPin, OUTPUT);
  pinMode(alertIndicatorPin, OUTPUT);
  pinMode(serverConnection, OUTPUT);

}
void wifiConfigAndMDNS() {
  WiFiManager wifiManager;
  WiFi.mode(WIFI_STA);

  if (MDNS.begin("alermsystem")) { // Start the mDNS responder for alermsystem.local
    Serial.println("mDNS responder started");
  }

  // This will attempt to connect using stored credentials
  if (!wifiManager.autoConnect("Alerm System")) {
    Serial.println("Failed to connect and hit timeout");
    ESP.restart(); // Restart the ESP if connection fails
    delay(1000);
  }

  // Now we will load the JSON array of cities from EEPROM
  loadCitiesFromEEPROM(); // Call the function that loads cities from EEPROM

  // If no saved cities were found, we can set a default value or leave it empty
  if (savedCitiesJson.isEmpty()) {
    savedCitiesJson = "[]"; // Default to an empty JSON array if needed
    Serial.println("No saved cities in EEPROM, using default empty array.");
  }
}


void connectToAlertServer() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    WiFiClientSecure client; // Use WiFiClientSecure for HTTPS
    client.setInsecure(); // Optionally, disable certificate verification
    http.begin(client, apiUrlAlert); // Pass the WiFiClientSecure and the URL
    int httpCode = http.GET(); // Make the request
    if (httpCode > 0) { // Check the returning code
      digitalWrite(serverConnection, HIGH);
      Serial.println("connect to alert server");
      String payload = http.getString(); // Get the request response payload
      Serial.println(payload); // Print the response payload
    }

    http.end(); // Close connection
  }
}

void handleRoot() {
  String htmlContent = R"(
<!DOCTYPE html>
<html lang='en'>
<head>
    <meta charset='UTF-8'>
    <title>Cities</title>
    <style>
        #filterInput {
            margin-bottom: 20px;
            padding: 10px;
            width: calc(90% - 22px);
        }
    </style>
</head>
<body>
    <h1>Select Cities</h1>
    <input type='text' id='filterInput' placeholder='Filter cities...'>
    <form id='cityForm'>
        <div id='cityList'></div>
        <input type='submit' value='Save Cities'>
    </form>
    <script>
        document.getElementById('cityForm').onsubmit = function(event) {
            event.preventDefault();
            var checkedBoxes = document.querySelectorAll('input[name=city]:checked');
            var targetCities = Array.from(checkedBoxes).map(function(box) { return box.value; });
            fetch('/save-cities', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ cities: targetCities })
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
            var labels = document.getElementById('cityList').getElementsByTagName('label');
            Array.from(labels).forEach(function(label) {
                var text = label.textContent || label.innerText;
                if (text.toUpperCase().indexOf(filter) > -1) {
                    label.style.display = '';
                } else {
                    label.style.display = 'none';
                }
            });
        };

        fetch('https://alerm-script.onrender.com/citiesjson')
        .then(function(response) { return response.json(); })
        .then(function(cities) {
            var cityListContainer = document.getElementById('cityList');
            cities.forEach(function(city) {
                var label = document.createElement('label');
                var checkbox = document.createElement('input');
                checkbox.type = 'checkbox';
                checkbox.name = 'city';
                checkbox.value = city;
                label.appendChild(checkbox);
                label.appendChild(document.createTextNode(city));
                label.appendChild(document.createElement('br'));
                cityListContainer.appendChild(label);
            });
        })
        .catch(function(error) {
            console.error('Error fetching the cities:', error);
        });
    </script>
</body>
</html>
)";
  server.send(200, "text/html", htmlContent);
}


void handleDisplaySavedCities() {
  // Ensure this variable is globally declared and populated correctly
  Serial.println("Saved cities JSON: " + savedCitiesJson); // For debugging

  // Start the HTML response
  String responseHtml = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><title>Selected Cities</title></head><body>";
  responseHtml += "<h1>Selected Cities</h1><ul>" +savedCitiesJson;

  // End the HTML response
  responseHtml += "</ul><a href='/'><button>Return</button></a></body></html>";

  // Send the response
  server.send(200, "text/html", responseHtml);
}



// Save cities to EEPROM
void saveCitiesToEEPROM(const String& cities) {
  // Clear EEPROM
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  // Write cities to EEPROM
  for (unsigned int i = 0; i < cities.length(); i++) {
    EEPROM.write(CITY_ADDRESS + i, cities[i]);
  }
  EEPROM.commit(); // Save changes to EEPROM
}

void handleSaveCities() {
  if (server.hasArg("plain")) {
    String requestBody = server.arg("plain");
    Serial.println("Received cities to save: " + requestBody);

    // Save the JSON string of cities to EEPROM
    saveCitiesToEEPROM(requestBody);

    // Save the JSON string in the global variable for immediate use
    savedCitiesJson = requestBody;

    // Redirect to the GET route to display saved cities
    server.sendHeader("Location", "/save-cities", true);
    server.send(302, "text/plain", ""); // HTTP 302 Redirect
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}
// Load cities from EEPROM
void loadCitiesFromEEPROM() {
  savedCitiesJson = ""; // Clear current data
  for (unsigned int i = CITY_ADDRESS; i < EEPROM_SIZE; i++) {
    char readValue = char(EEPROM.read(i));
    if (readValue == 0) { // Null character signifies end of data
      break;
    }
    savedCitiesJson += readValue; // Append to the savedCitiesJson string
  }
  Serial.println("Loaded cities from EEPROM: " + savedCitiesJson);
}