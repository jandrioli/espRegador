#include <ArduinoJson.h>
#include "FS.h"
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

//
// Physical connections 
//
#define HW_BATT   A3       // a input
#define HW_TEMP   A0       // a input
#define HW_BUZZ   3        // d output
#define HW_BUTT1  A1       // d input
#define HW_BUTT2  A2       // d input
#define HW_WATE   6        // d output
#define HW_RELAY1 D5       // d output
#define HW_RELAY2 D6       // d output
#define HW_RELAY3 D7       // d output
#define HW_RELAY4 D8       // d output
#define HW_CSN    9        // icsp
#define HW_CE    10        // icsp

#define useCredentialsFile
#ifdef useCredentialsFile
#include "credentials.h"
#else
mySSID = "    ";
myPASSWORD = "   ";
#endif
// SW Logic and firmware definitions
// 
#define THIS_NODE_ID 3                  // master is 0, unoR3 debugger is 1, promicro_arrosoir is 2, etc
#define DEFAULT_ACTIVATION 600          // 10h from now we activate (in case radio is down and can't program)
#define DEFAULT_DURATION 10             // max 10s of activation time by default

/**
 * exchange data via radio more efficiently with data structures.
 * we can exchange max 32 bytes of data per msg. 
 * schedules are reset every 24h (last for a day) so an INTEGER is
 * large enough to store the maximal value of a 24h-schedule.
 * temperature threshold is rarely used
 */
struct relayctl {
  unsigned long uptime = 0;                      // current running time of the machine (millis())  4 bytes  
  unsigned long sched1 = DEFAULT_ACTIVATION;     // schedule in minutes for the relay output nbr1   4 bytes
  unsigned long sched2 = DEFAULT_ACTIVATION+1;   // schedule in minutes for the relay output nbr2   4 bytes
  unsigned int  maxdur1 = DEFAULT_DURATION;      // max duration nbr1 is ON                         2 bytes
  unsigned int  maxdur2 = DEFAULT_DURATION;      // max duration nbr2 is ON                         2 bytes
  unsigned long sched3 = DEFAULT_ACTIVATION+2;   // schedule in minutes for the relay output nbr1   4 bytes
  unsigned long sched4 = DEFAULT_ACTIVATION+3;   // schedule in minutes for the relay output nbr2   4 bytes
  unsigned int  maxdur3 = DEFAULT_DURATION;      // max duration nbr1 is ON                         2 bytes
  unsigned int  maxdur4 = DEFAULT_DURATION;      // max duration nbr2 is ON                         2 bytes
  unsigned int  temp_thres = 999;                // temperature at which the syatem is operational  4 bytes
  float         temp_now   = 20;                 // current temperature read on the sensor          4 bytes
  short         battery    =  0;                 // current temperature read on the sensor          2 bytes
  bool          state1 = false;                  // state of relay output 1                         1 byte
  bool          state2 = false;                  // "" 2                                            1 byte
  bool          state3 = false;                  // state of relay output 1                         1 byte
  bool          state4 = false;                  // "" 2                                            1 byte
  bool          waterlow = false;                // indicates whether water is low                  1 byte
  byte          nodeid = 3;           // nodeid is the identifier of the slave           1 byte
} myData;

String g_nwSSID = "", g_nwPASS = "";
ESP8266WebServer server ( 80 );



bool loadConfig() 
{
  Serial.println("Loading configuration...");
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(buf.get(), size);

  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    Serial.println("Failed to parse config file");
    return false;
  }

  const char* nwSSID = json["ssid"];
  const char* nwPASS = json["pass"];
  g_nwSSID = String(nwSSID);
  g_nwPASS = String(nwPASS);
  Serial.println("["+g_nwSSID+"]");  
  Serial.println("["+g_nwPASS+"]");
  
  if (json.containsKey("sched1") )
  {
    myData.sched1 = json["sched1"];
    myData.sched2 = json["sched2"];
    myData.sched3 = json["sched3"];
    myData.sched4 = json["sched4"];
    myData.maxdur1 = json["maxdur1"];
    myData.maxdur2 = json["maxdur2"];
    myData.maxdur3 = json["maxdur3"];
    myData.maxdur4 = json["maxdur4"];
  }

  if (g_nwSSID.length() < 4 || g_nwPASS.length() < 6)
  {
    Serial.println("SSID or PSK were too short, defaulting to hard-coded nw.");
    g_nwSSID = mySSID;
    g_nwPASS = myPASSWORD;
  }
  return true;
}

bool saveConfig() 
{
  Serial.println("Saving configuration into spiffs...");
  char cSSID[g_nwSSID.length()+1], cPASS[g_nwPASS.length()+1];
  g_nwSSID.toCharArray(cSSID, g_nwSSID.length()+1);    
  g_nwPASS.toCharArray(cPASS, g_nwPASS.length()+1);    
  Serial.print("Saving new SSID:[");
  Serial.print(cSSID);
  Serial.println(']');
  Serial.print("Saving new PASS:[");
  Serial.print(cPASS);
  Serial.println(']');
  
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["ssid"] = cSSID;
  json["pass"] = cPASS;
  
  json["sched1"] = myData.sched1;
  json["sched2"] = myData.sched2;
  json["sched3"] = myData.sched3;
  json["sched4"] = myData.sched4;
  
  json["maxdur1"] = myData.maxdur1;
  json["maxdur2"] = myData.maxdur2;
  json["maxdur3"] = myData.maxdur3;
  json["maxdur4"] = myData.maxdur4;
  
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return false;
  }
  json.printTo(configFile);
  return true;
}



void setup_wifi() 
{
  delay(10);

  // Connect to WiFi network
  Serial.println(F("Connecting"));
    
    
  WiFi.mode(WIFI_STA);
  char cSSID[g_nwSSID.length()+1], cPASS[g_nwPASS.length()+1];
  g_nwSSID.toCharArray(cSSID, g_nwSSID.length()+1);    
  g_nwPASS.toCharArray(cPASS, g_nwPASS.length()+1);    
  WiFi.begin(cSSID, cPASS);

  int timeout = 40;
  while (WiFi.status() != WL_CONNECTED) 
  {
    Serial.print(".");
    if (timeout == 30) // a basic connect timeout sorta thingy
    {
      Serial.println();
      WiFi.printDiag(Serial);
      Serial.print("Failed to connect to WiFi nw. Status is now ");
      Serial.println(WiFi.status());
      Serial.println(F("Connecting first hardcoded wifi network"));
      WiFi.begin(mySSID, myPASSWORD);
    }
    if (timeout == 20) // a basic connect timeout sorta thingy
    {
      Serial.println();
      WiFi.printDiag(Serial);
      Serial.print("Failed to connect to WiFi nw. Status is now ");
      Serial.println(WiFi.status());
      Serial.println(F("Connecting secondary hardcoded wifi network"));
      WiFi.begin(mySSID2, myPASSWORD2);
    }
    if (timeout == 10) // a basic connect timeout sorta thingy
    {
      Serial.println();
      WiFi.printDiag(Serial);
      Serial.print("Failed to connect to WiFi nw. Status is now ");
      Serial.println(WiFi.status());
      Serial.println(F("Connecting thirdly hardcoded wifi network"));
      WiFi.begin(mySSID3, myPASSWORD3);
    }
    if (--timeout < 1) // a basic authentication-timeout sorta thingy
    {
      break;
    }
    delay(1000);
  }
  
  Serial.println(F(""));
  Serial.println(F("WiFi connected"));
  Serial.print(F("IP address: "));
  Serial.println(WiFi.localIP());  
  WiFi.printDiag(Serial);
}

void setup_spiffs()
{
  uint32_t realSize = ESP.getFlashChipRealSize();
  uint32_t ideSize = ESP.getFlashChipSize();
  FlashMode_t ideMode = ESP.getFlashChipMode();

  printf("Flash real id:   %08X\n", ESP.getFlashChipId());
  printf("Flash real size: %u\n\n", realSize);

  printf("Flash ide  size: %u\n", ideSize);
  printf("Flash ide speed: %u\n", ESP.getFlashChipSpeed());
  printf("Flash ide mode:  %s\n", (ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT" : ideMode == FM_DIO ? "DIO" : ideMode == FM_DOUT ? "DOUT" : "UNKNOWN"));
  if(ideSize != realSize) 
  {
      Serial.println("Flash Chip configuration wrong!\n");
  } 
  else 
  {
      Serial.println("Flash Chip configuration ok.\n");
      Serial.println("Mounting SPIFFS...");
      if (!SPIFFS.begin()) {
        Serial.println("Failed to mount file system");
        return;
      }
      if (!loadConfig()) {
        Serial.println("Failed to load config");
      } else {
        Serial.println("Config loaded");
      }
  }
  Serial.println(F("- - - - -"));  
}


String getPage()
{
  String page = "<html lang='br'><head><meta http-equiv='refresh' content='60' name='viewport' content='width=device-width, initial-scale=1'/>";
  page += "<link rel='stylesheet' href='https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap.min.css'><script src='https://ajax.googleapis.com/ajax/libs/jquery/3.1.1/jquery.min.js'></script><script src='https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/js/bootstrap.min.js'></script>";
  page += "<title>Regador Wifi da Lourdes</title></head><body>";
  page += "<div class='container-fluid'>";
  page +=   "<div class='row'>";
  page +=     "<div class='col-md-12'>";
  page +=       "<h1>Regador Wifi</h1>";
  page +=       "<h3>Mini estacao de controle de hidratacao dos seres verdes</h3>";
  page +=       "<ul class='nav nav-pills'>";
  page +=         "<li class='active'>";
  page +=           "<a href='#'> <span class='badge pull-right'>";
  page +=           myData.temp_now;
  page +=           "</span> Temperatura</a>";
  page +=         "</li><li>";
  page +=           "<a href='#'> <span class='badge pull-right'>";
  page +=           myData.waterlow;
  page +=           "</span> N&iacute;vel de &aacute;gua</a>";
  page +=         "</li><li>";
  page +=           "<a href='#'> <span class='badge pull-right'>";
  page +=           myData.uptime;
  page +=           "</span> Horas online</a></li>";
  page +=       "</ul><form action='/' method='POST'><h4>Programa&ccedil;&atilde;o di&aacute;ria: esta tabela determina daqui h&aacute; quanto tempo a irriga&ccedil;&atilde;o come&ccedil;a.</h4>";
  page +=       "<table class='table'>";  // Tableau des relevés
  page +=         "<thead><tr><th>Rel&eacute;</th><th tooltip='Intervalo em minutos entre cada ativacao'>Intervalo</th><th>Dura&ccedil;&atilde;o</th></tr></thead>"; //Entête
  page +=         "<tbody>";  // Contenu du tableau
  page +=           "<tr";
  if (myData.state1) page += " class='active'";
  page +=             "><td>1</td><td><input type=text size=4 maxlength=4 value="; // Première ligne : température
  page +=             myData.sched1;
  page +=             " name=sched1 size=4 />min</td><td><input type=text value=" ; 
  page +=             myData.maxdur1;
  page +=             " name=maxdur1 size=2 maxlength=2 />s</td></tr>";
  page +=           "<tr";
  if (myData.state2) page += " class='active'";
  page +=             "><td>2</td><td><input type=text maxlength=4 value="; // 2nd ligne : Humidité
  page +=             myData.sched2;
  page +=             " name=sched2 size=4 />min</td><td><input type=text value=" ;
  page +=             myData.maxdur2;
  page +=             " name=maxdur2 size=2 maxlength=2 />s</td></tr>";
  page +=           "<tr";
  if (myData.state3) page += " class='active'";
  page +=             "><td>3</td><td><input type=text maxlength=4 value="; // 3ème ligne : PA (BMP180)
  page +=             myData.sched3;
  page +=             " name=sched3 size=4 />min</td><td><input type=text value=";
  page +=             myData.maxdur3;
  page +=             " name=maxdur3 size=2 maxlength=2 />s</td></tr>";
  page +=           "<tr";
  if (myData.state4) page += " class='active'";
  page +=             "><td>4</td><td><input type=text maxlength=4 value="; // 4th relay
  page +=             myData.sched4;
  page +=             " name=sched4 size=4 />min</td><td><input type=text value=";
  page +=             myData.maxdur4;
  page +=             " name=maxdur4 size=2 maxlength=2 />s</td></tr>";
  page +=       "</tbody></table>";
  page +=       "<button type='button submit' name='sched' value='1' class='btn btn-success btn-lg'>Salvar</button></form>";
  page +=       "<h3>Acionamento manual</h3>";
  page +=       "<div class='row'>";
  page +=         "<div class='col-md-4'><h4 class ='text-left'>D5 ";
  page +=         "<span class='badge'>";
  page +=         (myData.state1?"ON":"OFF");
  page +=         "</span><form action='/' method='POST'><button type='button submit' name='D5' ";
  if (!myData.state1) 
  page +=         "value='1' class='btn btn-success btn-lg'>ON</button></form>";
  else
  page +=         "value='0' class='btn btn-danger btn-lg'>OFF</button></form>";
  page +=         "</h4></div>";
  page +=         "<div class='col-md-4'><h4 class ='text-left'>D6 <span class='badge'>";
  page +=         (myData.state2?"ON":"OFF");
  page +=         "</span><form action='/' method='POST'><button type='button submit' name='D6' ";
  if (!myData.state2) 
  page +=         "value='1' class='btn btn-success btn-lg'>ON</button></form>";
  else
  page +=         "value='0' class='btn btn-danger btn-lg'>OFF</button></form>";
  page +=         "</h4></div>";
  page +=         "<div class='col-md-4'><h4 class ='text-left'>D7 <span class='badge'>";
  page +=         (myData.state3?"ON":"OFF");
  page +=         "</span><form action='/' method='POST'><button type='button submit' name='D7' ";
  if (!myData.state3) 
  page +=         "value='1' class='btn btn-success btn-lg'>ON</button></form>";
  else
  page +=         "value='0' class='btn btn-danger btn-lg'>OFF</button></form>";
  page +=         "</h4></div>";
  page +=         "<div class='col-md-4'><h4 class ='text-left'>D8 <span class='badge'>";
  page +=         (myData.state4?"ON":"OFF");
  page +=         "</span><form action='/' method='POST'><button type='button submit' name='D8' ";
  if (!myData.state4) 
  page +=         "value='1' class='btn btn-success btn-lg'>ON</button></form>";
  else
  page +=         "value='0' class='btn btn-danger btn-lg'>OFF</button></form>";
  page +=       "</h4></div>";
  page += "</div></div></div>";
  page += "</body></html>";
  return page;
}
void handleRoot()
{
  if ( server.hasArg("D5") ) {
    handleD5();
  } else if ( server.hasArg("D6") ) {
    handleD6();
  } else if ( server.hasArg("D7") ) {
    handleD7();
  } else if ( server.hasArg("D8") ) {
    handleD8();
  } else if ( server.hasArg("sched") ) {
    handleSched();
    saveConfig();
  }
  server.send ( 200, "text/html", getPage() );
}

void handleSched()
{
  String s1,s2,s3,s4,d1,d2,d3,d4;
  s1 = server.arg("sched1");
  s2 = server.arg("sched2");
  s3 = server.arg("sched3");
  s4 = server.arg("sched4");
  d1 = server.arg("maxdur1");
  d2 = server.arg("maxdur2");
  d3 = server.arg("maxdur3");
  d4 = server.arg("maxdur4");
  Serial.println("Params: " + s1 + " " + s2 + " " + s3 + " " + s4 + " " + d1 + " " + d2 + " " + d3 + " " + d4);
  myData.sched1 = s1.toInt();
  myData.sched2 = s2.toInt();
  myData.sched3 = s3.toInt();
  myData.sched4 = s4.toInt();
  myData.maxdur1 = d1.toInt();
  myData.maxdur2 = d2.toInt();
  myData.maxdur3 = d3.toInt();
  myData.maxdur4 = d4.toInt();
}
void handleD5() {
  String D5Value = server.arg("D5"); 
  if ( D5Value.toInt() ) 
    myData.state1 = true;
  else 
    myData.state1 = false;
}
 
void handleD6() {
  String D6Value = server.arg("D6"); 
  if ( D6Value.toInt() ) 
    myData.state2 = true;
  else 
    myData.state2 = false;
}
 
void handleD7() {
  String D7Value = server.arg("D7"); 
  if ( D7Value.toInt() ) 
    myData.state3 = true;
  else 
    myData.state3 = false;
}
 
void handleD8() {
  String D8Value = server.arg("D8"); 
  if ( D8Value.toInt() ) 
    myData.state4 = true;
  else 
    myData.state4 = false;
}


void setup() 
{
  pinMode(HW_RELAY1, OUTPUT);
  pinMode(HW_RELAY2, OUTPUT);
  pinMode(HW_RELAY3, OUTPUT);
  pinMode(HW_RELAY4, OUTPUT);
  delay(500);
  //
  // Print preamble
  //
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("========================"));  
  Serial.println(F("  REGADOR WIFI ESP-12E  "));  
  Serial.println(F("========================"));  
  delay(500);
  Serial.println(F("Warning! Always query the controller node before attempting to program it!"));  
  delay(500);
  Serial.println(F("If you have trouble interfacing, try the Serial interface to configure me."));  
  delay(500);
  Serial.println(F("- - - - -"));  
  
  
  //prepare and configure SPIFFS
  setup_spiffs();
  
  // setting up WLAN related stuff 
  setup_wifi();

  // On branche la fonction qui gère la premiere page / link to the function that manage launch page 
  server.on ( "/", handleRoot );
  server.on ( "", handleRoot );
 
  server.begin();  
  Serial.println ( "HTTP server started" );
}

long t = 0;
void loop() {
  // put your main code here, to run repeatedly:

  server.handleClient();

  if ( millis() - t > 10000 )
  {
    myData.waterlow = !digitalRead(HW_WATE);
    myData.uptime = (float)millis() / (float)60000;
    t = millis();
    Serial.println("alive");
  }
  // activating relays
  {
    if ( millis()/60000 >= myData.sched1  && myData.sched1 > 0  )
    {
      if (myData.state1 = false) Serial.println("Activating plug1");
      myData.state1 = true;
    }
    if ( millis()/60000 >= myData.sched2  && myData.sched2 > 0  )
    {
      if (myData.state2 = false) Serial.println("Activating plug2");
      myData.state2 = true;
    }
    if ( millis()/60000 >= myData.sched3  && myData.sched3 > 0  )
    {
      if (myData.state3 = false) Serial.println("Activating plug3");
      myData.state3 = true;
    }
    if ( millis()/60000 >= myData.sched4  && myData.sched4 > 0  )
    {
      if (myData.state4 = false) Serial.println("Activating plug4");
      myData.state4 = true;
    }
  }
    
  // switch relays off after max_duration, & sendout new status
  if ( myData.sched1 > 0 && (millis()/1000) > (myData.sched1*60)+myData.maxdur1 )
  { 
    Serial.println("Deactivating plug1"); 
    myData.state1 = false;
    //automatically schedule relay1 to tomorrow
    myData.sched1 += 1440;
  }
  if ( myData.sched2 > 0 && (millis()/1000) > (myData.sched2*60)+myData.maxdur2 )
  { 
    Serial.println("Deactivating plug2"); 
    myData.state2 = false;
    //automatically schedule relay2 to tomorrow
    myData.sched2 += 1440; 
  }
  if ( myData.sched3 > 0 && (millis()/1000) > (myData.sched3*60)+myData.maxdur3 )
  { 
    Serial.println("Deactivating plug3"); 
    myData.state3 = false;
    //automatically schedule relay1 to tomorrow
    myData.sched3 += 1440;
  }
  if ( myData.sched4 > 0 && (millis()/1000) > (myData.sched4*60)+myData.maxdur4 )
  { 
    Serial.println("Deactivating plug4"); 
    myData.state4 = false;
    //automatically schedule relay2 to tomorrow
    myData.sched4 += 1440; 
  }


  
  /* 
   *  Because I dunno how I will eventually need to debug this thing,
   *  I am placing serial iface support here too. Sucks. 
   */
  while (Serial.available())
  {
    String s1 = Serial.readStringUntil('\n');
    Serial.println(F("CAREFUL, end of line is only NL and no CR!!!"));
    Serial.print("You typed:");
    Serial.println(s1);
    if (s1.indexOf("setnewssid ")>=0)
    {
      s1 = s1.substring(s1.indexOf(" ")+1);
      g_nwSSID = s1.substring(0, s1.length());
      Serial.println(("new ssid is now [" + g_nwSSID + "]"));
    }
    else if (s1.indexOf("setnewpass ")>=0)
    {
      s1 = s1.substring(s1.indexOf(" ")+1);
      g_nwPASS = s1.substring(0, s1.length());
      Serial.println(("new pass is now [" + g_nwPASS + "]"));
    }
    else if (s1.indexOf("save")>=0)
    {
      saveConfig();
    }
    else if ((s1.indexOf("setnewpass")!=0) && (s1.indexOf("setnewssid")!=0) && (s1.indexOf("setnewmqtt")!=0))
    {
      Serial.println("** Serial interface expects:\n\r"\
        "** 0 - setnewssid: set a new SSID for the module\n\r"\
        "** 1 - setnewpass: set a new PSK key for the nw\n\r"\
        "** 3 - save : save the configuration into a file on the flash");
    }
  }
}
