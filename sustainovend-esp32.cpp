/* Licensed under the GNU GPL 3.0 */

#include <Arduino.h>
#define ENABLE_SERVICE_AUTH
#define ENABLE_FIRESTORE
#define ENABLE_FIRESTORE_QUERY

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <WiFiManager.h>

#include <ArduinoJson.h>

#include "secrets.h"

#define FIREBASE_PROJECT_ID "YOUR_FIREBASE_PROJECT"
#define FIREBASE_CLIENT_EMAIL "YOUR_FIREBASE_CLIENT_EMAIL"

#define VENDID "vend1"
FirebaseApp app;
WiFiClientSecure ssl_client;

using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

ServiceAuth sa_auth(FIREBASE_CLIENT_EMAIL, FIREBASE_PROJECT_ID, API_KEY, 3000 /* expire period in seconds (<3600) */);
Firestore::Documents Docs;

bool done = false;

struct UserData {
  String docPath;
  String name;
  String id;
  int points;
  bool valid;
};

UserData currentDoc = {"", "", "", 0, false};

String query_await(const String &documentPath, QueryOptions &queryOptions);
String update_document_await(const String &documentPath, PatchDocumentOptions &patchOptions, Document<Values::Value> &doc);
void processData(AsyncResult &aResult);

HardwareSerial mySerial(2);
String userDataToJson(const UserData &data) {
  JsonDocument json;
  json["docPath"] = data.docPath;
  json["name"] = data.name;
  json["id"] = data.id;
  json["points"] = data.points;
  String out;
  serializeJson(json, out);
  return out;
}

void stripFirstAndLast(char *str) {
  int len = strlen(str);  // get string length
  if (len <= 2) {         // nothing to strip
    str[0] = '\0';
    return;
  }

  // Shift characters left by 1 to remove first character
  for (int i = 0; i < len - 2; i++) {
    str[i] = str[i + 1];
  }

  // Null-terminate before the last character
  str[len - 2] = '\0';
}

uint32_t get_ntp_time() {
  uint32_t ts = 0;
  Serial.print("Getting time from NTP server... ");
#if defined(ESP8266) || defined(ESP32) || defined(CORE_ARDUINO_PICO)
  int max_try = 10, retry = 0;
  while (time(nullptr) < FIREBASE_DEFAULT_TS && retry < max_try) {
    configTime(3 * 3600, 0, "pool.ntp.org");
    unsigned long m = millis();
    while (time(nullptr) < FIREBASE_DEFAULT_TS && millis() - m < 10 * 1000) {
      delay(100);
      ts = time(nullptr);
    }
    Serial.print(ts == 0 ? " failed, retry... " : "");
    retry++;
  }
  ts = time(nullptr);
#elif __has_include(<WiFiNINA.h>) || __has_include(<WiFi101.h>)
  ts = WiFi.getTime();
#endif

  Serial.println(ts > 0 ? "success" : "failed");
  return ts;
}


String addPointsToId(const char *docPath, int newPoints) {
  String docPathStr = String(docPath);

  int lastSlash = docPathStr.lastIndexOf('/');
  String idName = lastSlash >= 0 ? docPathStr.substring(lastSlash + 1) : docPathStr;

  String docPathFull = String(VENDID) + "/" + idName;

  Values::IntegerValue intV(newPoints);
  Document<Values::Value> doc("points", Values::Value(intV));
  PatchDocumentOptions patchOptions(DocumentMask("points"), DocumentMask(), Precondition());

  return update_document_await(docPathFull, patchOptions, doc);
}

UserData getDocFromId(const String &Id) {
  StructuredQuery query;
  query.select(Projection(FieldReference("id")).add(FieldReference("name")).add(FieldReference("points")));
  query.from(CollectionSelector(VENDID, false));

  FieldFilter fieldFilter;
  Values::StringValue stringValue(Id);
  fieldFilter.field(FieldReference("id")).op(FieldFilterOperator::EQUAL).value(Values::Value(stringValue));

  query.where(Filter(fieldFilter));
  query.limit(1);

  QueryOptions queryOptions;
  queryOptions.structuredQuery(query);

  UserData err = {"", "", "", 0, false};
  String response = query_await("", queryOptions);

  if (response.length() == 0) {
    Serial.println("query_await returned empty response!");
    return err;
  }

  response = response.substring(1, response.length() - 1);

  JsonDocument currentDocJson;
  DeserializationError error = deserializeJson(currentDocJson, response);
  if (error) {
    Serial.print("JSON parse failed: ");
    Serial.println(error.c_str());
    return err;
  }

  JsonObject jsonDoc = currentDocJson["document"].as<JsonObject>();
  if (!jsonDoc) {
    Serial.println("No document in JSON!");
    return err;
  }

  JsonObject fields = jsonDoc["fields"].as<JsonObject>();
  if (!fields) {
    Serial.println("No fields in document!");
    return err;
  }

  UserData data;
  data.docPath = jsonDoc["name"] | "";
  data.name = fields["name"]["stringValue"] | "";
  data.id = fields["id"]["stringValue"] | "";
  const char *pointsStr = fields["points"]["integerValue"] | "";
  data.points = pointsStr[0] ? atoi(pointsStr) : 0;
  data.valid = true;

  queryOptions.clear();
  query.clear();
  return data;
}

String query_await(const String &documentPath, QueryOptions &queryOptions) {
  Serial.println("Querying a Firestore database...");

  String payload = Docs.runQuery(aClient, Firestore::Parent(FIREBASE_PROJECT_ID), documentPath, queryOptions);

  if (aClient.lastError().code() == 0) {
    return payload;
  }

  return "";
}


String update_document_await(const String &documentPath, PatchDocumentOptions &patchOptions, Document<Values::Value> &doc) {
  Serial.println("Updating a document...");

  String payload = Docs.patch(aClient, Firestore::Parent(FIREBASE_PROJECT_ID), documentPath, patchOptions, doc);

  if (aClient.lastError().code() == 0) {
    return payload;
  }

  return "";
}

void handleSerialCommands() {
  while (mySerial.available()) {
    String command = mySerial.readStringUntil('\n');
    Serial.println(command);
    command.trim();
    if (command.length() == 0) {
      continue;
    }
    if (ssl_client.connected() == 0) {
      Serial.println("not connected");
      mySerial.println("#ERR");
      continue;
    }
    if (command.startsWith("#LOADID-")) {
      String id = command.substring(8);
      UserData doc = getDocFromId(id);
      if (!doc.valid) {
        mySerial.println("#ERR");
        continue;
      }
      currentDoc = doc;
      currentDoc.valid = true;
      mySerial.print("#NAME-");
      mySerial.println(currentDoc.name);
      mySerial.print("#POINTS-");
      mySerial.println(currentDoc.points);
      continue;
    }

    if (command.startsWith("#ADD-")) {
      if (!currentDoc.valid) {
        mySerial.println("#ERR");
        continue;
      }
      String amountStr = command.substring(5);
      if (amountStr.length() == 0) {
        mySerial.println("#ERR");
        continue;
      }
      int delta = amountStr.toInt();
      int newPoints = currentDoc.points + delta;

      addPointsToId(currentDoc.docPath.c_str(), newPoints);
      currentDoc.points = newPoints;
      mySerial.println("#DONE");
      continue;
    }

    if (command == "#CLEARID") {
      currentDoc = {"", "", "", 0, false};
      mySerial.println("#DONE");
      delay(100);
      //ESP.restart();
      continue;
    }

    if (command == "#ISREADY") {
      mySerial.println(app.ready() ? "#READY" : "#NOTREADY");
      continue;
    }

    // Unsupported commands error
    mySerial.println("#ERR");
  }
}

void setup() {
  Serial.begin(115200); // debug serial
  mySerial.begin(115200, SERIAL_8N1, 16, 17); // RX2=16, TX2=17 (ESP32)
  delay(100);

  WiFiManager wm;
  Serial.println("Starting WiFi Manager...");
  
  bool res = wm.autoConnect("ESP32-Setup");

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  if(!res) {
    Serial.println("Failed to connect");
  } else {
    Serial.println("Connected!");
  }
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  ssl_client.setInsecure();

  ssl_client.setTimeout(5000);
  ssl_client.setHandshakeTimeout(10);


  // Assign the valid time only required for authentication process with ServiceAuth and CustomAuth.
  app.setTime(get_ntp_time());
  Serial.println("Initializing app...");
  initializeApp(aClient, app, getAuth(sa_auth), processData, "authTask");
  Serial.println("Initialized");
  mySerial.println("#READY");

  app.getApp<Firestore::Documents>(Docs);
}
void processData(AsyncResult &aResult) {
}
void loop() {
  // To maintain the authentication and async tasks
  app.loop();

  handleSerialCommands();
}