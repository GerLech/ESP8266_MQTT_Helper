#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>

#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WebConfig.h>



//Konfigurationswerte für Netzwerkanmeldung
//und MQTT Broker
String param_base = "["
  "{"
  "'name':'ssid',"
  "'label':'SSID des WLAN',"
  "'type':"+String(INPUTTEXT)+","
  "'default':''"
  "},"
  "{"
  "'name':'pwd',"
  "'label':'WLAN Passwort',"
  "'type':"+String(INPUTPASSWORD)+","
  "'default':''"
  "},"
  "{"
  "'name':'broker',"
  "'label':'MQTT Broker',"
  "'type':"+String(INPUTTEXT)+","
  "'default':'raspberrypi4'"
  "},"
  "{"
  "'name':'user',"
  "'label':'MQTT User',"
  "'type':"+String(INPUTTEXT)+","
  "'default':'broker'"
  "},"
  "{"
  "'name':'mqpwd',"
  "'label':'MQTT Password',"
  "'type':"+String(INPUTPASSWORD)+","
  "'default':'Broker1'"
  "}]";

//Zusätzliches Messintervall für Sensoren mit
//periodischer Messung
String param_intv = "[{'"
  "name':'intervall',"
  "'label':'Messintervall(s)',"
  "'type':"+String(INPUTNUMBER)+","
  "'min':0,"
  "'max':3600,"
  "'default':'2'"
  "}]";

//Instanzen der importierten Klassen anlegen
//Konfiguration
WebConfig conf;
//Webserver
ESP8266WebServer server;
//MQTT Client
WiFiClient espClient;
PubSubClient client(espClient);

//Globale Variablen
boolean connected;         //Status der WLAN Verbindung
String clientId;           //ID für MQTT
uint32_t nextConnect = 0;  //Nächster Zeitpunkt für MQTT Verbindungsversuch
uint32_t last = 0;         //Nächster Zeitpunkt für periodische Messung
bool isSensor = true;      //Flag ob periodische Messung erfolgen soll
void (* onPublish)() = NULL;   //Callbackfunktion für periodische Messung
void (* onSubscribe)() = NULL; //Callbackfunktion zum Registrieren von Themen

//Falls möglich am lokalen WLAN anmelden
boolean initWiFi() {
    boolean connected = false;
    //Stationsmodus
    WiFi.mode(WIFI_STA);
    //wenn eine SSID konfiguriert wurde versuchen wir uns anzumelden
    if (strlen(conf.getValue("ssid")) != 0) {
      Serial.print("Verbindung zu ");
      Serial.print(conf.getValue("ssid"));
      Serial.println(" herstellen");
      //Verbindungsaufbau starten
      WiFi.begin(conf.getValue("ssid"),conf.getValue("pwd"));
      uint8_t cnt = 0;
      //10 Sekunden auf erfolgreiche Verbindung warten
      while ((WiFi.status() != WL_CONNECTED) && (cnt<20)){
        delay(500);
        Serial.print(".");
        cnt++;
      }
      Serial.println();
      //Wenn die Verbindung erfolgreich war, wird die IP-Adresse angezeigt
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("IP-Adresse = ");
        Serial.println(WiFi.localIP());
        connected = true;
      }
    }
    if (!connected) {
      //keine Verbindung, wir starten einen Access Point
      //damit wir auf die Konfiguration zugreifen können
      WiFi.mode(WIFI_AP);
      WiFi.softAP(conf.getApName(),"",1);
    }

    return connected;
}

//Verbindung zum Broker überprüfen und falls nötig wiederherstellen
void checkMQTT() {
  if (!client.connected() && (millis() > nextConnect)) {
    //Wir haben keine Verbindung, also Wiederherstellung erforderlich
    Serial.print("Versuche MQTT Verbindung zu ");
    Serial.print(conf.getValue("broker"));
    Serial.print(" mit ID ");
    Serial.println(clientId);
    //Beim Broker anmelden
    client.setServer(conf.getValue("broker"),1883);
    boolean con = false;
    if (conf.getValue("user")!="") {
      con = client.connect(clientId.c_str(),conf.getValue("user"),conf.getValue("mqpwd"));
    } else {
      con = client.connect(clientId.c_str());
    }
    if (con) {
      Serial.println("Verbunden");
      //Nach erfolgreicher wird falls sie gesetzt ist, die Callback Funktion
      //aufgerufen um eventuell Themen zu abboniereen
      if (onSubscribe) onSubscribe();
      if (client.connected()) {
        nextConnect=0;
      } else {
        Serial.println("Fehler");
        nextConnect = millis()+5000;
      }
    } else {
      Serial.print("Fehlgeschlagen, rc=");
      Serial.print(client.state());
      Serial.println(" Nächster Versuch in 5 s");
      nextConnect = millis()+5000;
    }
  }

}

//HTTP Request bearbeiten und Formular anzeigen
void handleRoot() {
  conf.handleFormRequest(&server);
}

//Setup von WLAN und MQTT Verbindung
//Laden der Konfiguration
void ESP_MQTT_setup(String extra, bool sensor) {
  //Wenn gesetzt, wird eine periodische Messung unterstützt
  isSensor = sensor;
  //Die JSON Zeichenkette für das Konfigurations-Formular
  //wird gesetzt
  conf.setDescription(param_base);
  //Intervall für periodische Messung
  if (isSensor) conf.addDescription(param_intv);
  //Zusätzliche Parameter für die Anwendung
  if (extra != "") conf.addDescription(extra);
  //Falls vorhanden wird die Konfiguration aus dewm SPIFFS geladen
  conf.readConfig();
  //Als MQTT Client-Id wird die MAC-Adresse verwendet
  clientId=WiFi.macAddress();
  clientId.replace(":","");
  //Die MQTT Verbindungsparameter werden gesetzt
  client.setServer(conf.getValue("broker"),1883);
  //Die Netzwerk-Verbindung wird hergestellt
  connected = initWiFi();
  //mdns wird gestartet
  char dns[30];
  sprintf(dns,"%s.local",conf.getApName());
  if (MDNS.begin(dns)) {
    Serial.println("MDNS responder gestartet");
  }
  //Der Web Server wird gestartet
  server.on("/",handleRoot);
  server.begin(80);

}

void ESP_MQTT_loop() {
  if (connected) {
    //wenn eine Verbindung besteht
    //wird die Verbindung zum Broker getestet
    checkMQTT();
    if (client.connected()) {
      //Auf neue Nachrichten vom Broker prüfen
      client.loop();
      if (isSensor) {
        long now = millis();
        if (now - last > (conf.getInt("intervall")+1) * 1000) {
          last = now;
          //wenn das Intervall abgelaufen ist und eine Callbackfunktion
          //gesetzt wurde, wird diese aufgerufen
          if (onPublish) onPublish();
        }
      }
    }
  }
  //Der Web Server prüft ob Anfragen vorliegen
  server.handleClient();
  //MDNS wird aktualisiert
  MDNS.update();
}
