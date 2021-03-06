/*
 * Regador da mae
 */

#include <ArduinoJson.h>
#include "FS.h" 
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266SSDP.h>
// #include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
// #include <TimeLib.h>
#include "esp.h"

//
// Physical connections 
//
//#define HW_LEDB    LED_BUILTIN
#define HW_LEDG    4            //  output
#define HW_LEDR    5            //  output
#define HW_WATER  13            //  input_pullup
#define HW_RELAY1 14            //  output
#define HW_RELAY2 12            //  output
// SW Logic and firmware definitions
// 
#define THIS_NODE_ID 3                  // master is 0, unoR3 debugger is 1, promicro_arrosoir is 2, etc
#define DEFAULT_ACTIVATION 600          // 10h from now we activate (in case radio is down and can't program)
#define DEFAULT_DURATION 10             // max 10s of activation time by default
#define DEFAULT_HEBDO 1                 // repeat program every 1 day 

#define useCredentialsFile
#ifdef useCredentialsFile
#include "credentials.h"
#else
mySSID = "    ";
myPASSWORD = "   ";
#endif
/**
 * exchange data via radio more efficiently with data structures.
 * we can exchange max 32 bytes of data per msg. 
 * schedules are reset every 24h (last for a day) so an INTEGER is
 * large enough to store the maximal value of a 24h-schedule.
 * temperature threshold is rarely used
 */
struct relayctl {
  uint32_t uptime = 0;                      // current running time of the machine (millis())  4 bytes  
  uint32_t sched1 = DEFAULT_ACTIVATION;     // schedule in minutes for the relay output nbr1   4 bytes
  uint32_t sched2 = DEFAULT_ACTIVATION;     // schedule in minutes for the relay output nbr2   4 bytes
  uint16_t maxdur1 = DEFAULT_DURATION;      // max duration nbr1 is ON                         2 bytes
  uint16_t maxdur2 = DEFAULT_DURATION;      // max duration nbr2 is ON                         2 bytes
   int8_t  temp_thres = 99;                 // temperature at which the syatem is operational  1 byte
   int8_t  temp_now   = 20;                 // current temperature read on the sensor          1 byte
  uint8_t  battery    =  0;                 // current temperature read on the sensor          1 byte
  bool     state1 = false;                  // state of relay output 1                         1 byte
  bool     state2 = false;                  // "" 2                                            1 byte
  bool     waterlow = false;                // indicates whether water is low                  1 byte
  uint8_t  nodeid = 64; /*SSDP*/            // nodeid is the identifier of the slave   (SSDP)  1 byte
} __attribute__ ((packed)) myData;

String                g_nwSSID = "", 
                      g_nwPASS = "",
                      g_tgCHAT = "60001082";
ESP8266WebServer      server ( 80 );
int                   Bot_mtbs = 10000; //if i lower this interval, heap collides with stack
unsigned long         Bot_lasttime = 0;   //last time messages' scan has been done
bool                  activation_notified = false;
const static char     m_sHR[]  = "- - - - -";
const static char     m_sBtnSuccess[]  = "btn-success";
const static char     m_sBtnDanger[]  = "btn-danger";
const static char     m_sClassActive[]  = "class='active'";
const static char     m_sON[]  = "ON";
const static char     m_sOFF[]  = "OFF";
uint8_t               m_ssidScan = 0;
uint8_t               m_hebdo = DEFAULT_HEBDO;
unsigned long         t = 0;
bool                  b = false, 
                      bRealState1 = false, 
                      bRealState2 = false;
WiFiClient            botclient;
UniversalTelegramBot  bot(BOTtoken, botclient);


bool loadConfig() 
{
  Serial.println(F("Loading configuration..."));
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println(F("Failed to open config file"));
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println(F("Config file size is too large"));
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
    Serial.println(F("Failed to parse config file"));
    return false;
  }

  if (json.containsKey("ssid")) 
  {
    //const char* nwSSID = json["ssid"];
    g_nwSSID = String((const char*)json["ssid"]);
  }
  if (json.containsKey("pass")) 
  {
    //const char* nwPASS = json["pass"];
    g_nwPASS = String((const char*)json["pass"]);
  }
  if (json.containsKey("chat")) 
  {
    //const char* tgCHAT = json["chat"];
    g_tgCHAT = String((const char*)json["chat"]);
  }
  Serial.println("["+g_nwSSID+"]");  
  Serial.println("["+g_nwPASS+"]");
  Serial.println("["+g_tgCHAT+"]");
  if (json.containsKey("sched1") )
  {
    myData.sched1 = json["sched1"];
    myData.sched2 = json["sched2"];
    myData.maxdur1 = json["maxdur1"];
    myData.maxdur2 = json["maxdur2"];
  }

  if (json.containsKey("hebdo") )
  {
    m_hebdo = (uint8_t)json["hebdo"];
  }
  
  if (g_nwSSID.length() < 4 || g_nwPASS.length() < 6)
  {
    Serial.println(F("SSID or PSK were too short, defaulting to hard-coded nw."));
    g_nwSSID = mySSID;
    g_nwPASS = myPASSWORD;
  }
  return true;
}

bool saveConfig() 
{
  Serial.println(F("Saving configuration into spiffs..."));
  char cSSID[g_nwSSID.length()+1], cPASS[g_nwPASS.length()+1], cCHAT[g_tgCHAT.length()+1];
  g_nwSSID.toCharArray(cSSID, g_nwSSID.length()+1);    
  g_nwPASS.toCharArray(cPASS, g_nwPASS.length()+1);    
  g_tgCHAT.toCharArray(cCHAT, g_tgCHAT.length()+1);
  Serial.print(F("Saving new SSID:["));
  Serial.print(cSSID);
  Serial.println(']');
  Serial.print(F("Saving new PASS:["));
  Serial.print(cPASS);
  Serial.println(']');
  Serial.print(F("Saving new CHAT:["));
  Serial.print(cCHAT);
  Serial.println(']');
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["ssid"] = cSSID;
  json["pass"] = cPASS;
  json["chat"] = cCHAT;
  
  json["sched1"] = myData.sched1;
  json["sched2"] = myData.sched2;
  
  json["maxdur1"] = myData.maxdur1;
  json["maxdur2"] = myData.maxdur2;
  json["hebdo"] = m_hebdo;
  
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println(F("Failed to open config file for writing"));
    return false;
  }
  json.printTo(configFile);
  return true;
}



void setup_wifi() 
{
  WiFi.mode(WIFI_OFF);
  yield();
  b = !b;
    
  delay(10);

  // Connect to WiFi network
  Serial.println(F("Connecting WiFi..."));
  delay(20); 
  WiFi.mode(WIFI_STA);
  yield();
  delay(20); 
  
  char cSSID[g_nwSSID.length()+1], cPASS[g_nwPASS.length()+1];
  g_nwSSID.toCharArray(cSSID, g_nwSSID.length()+1);    
  g_nwPASS.toCharArray(cPASS, g_nwPASS.length()+1);    
  WiFi.begin(cSSID, cPASS);
  yield();

  int timeout = 20;
  while (WiFi.status() != WL_CONNECTED) 
  {
    digitalWrite(HW_LEDR, HIGH);
    delay(100);
    digitalWrite(HW_LEDR, LOW);
    delay(900);
    
    if (timeout == 15) // a basic connect timeout sorta thingy
    {
      Serial.println();
      WiFi.printDiag(Serial);
      Serial.print(F("Failed to connect to WiFi nw. Status is now "));
      Serial.println(WiFi.status());
      Serial.println(F("Connecting first hardcoded wifi network"));
      WiFi.mode(WIFI_OFF);
      yield();
      delay(50); 
      WiFi.begin(mySSID, myPASSWORD);
    }
    if (timeout == 10) // a basic connect timeout sorta thingy
    {
      Serial.println();
      WiFi.printDiag(Serial);
      Serial.print(F("Failed to connect to WiFi nw. Status is now "));
      Serial.println(WiFi.status());
      Serial.println(F("Connecting secondary hardcoded wifi network"));
      WiFi.mode(WIFI_OFF);
      yield();
      delay(50); 
      WiFi.begin(mySSID2, myPASSWORD2);
    }
    if (timeout == 5) // a basic connect timeout sorta thingy
    {
      Serial.println();
      WiFi.printDiag(Serial);
      Serial.print(F("Failed to connect to WiFi nw. Status is now "));
      Serial.println(WiFi.status());
      Serial.println(F("Connecting thirdly hardcoded wifi network"));
      WiFi.mode(WIFI_OFF);
      yield();
      delay(50); 
      WiFi.begin(mySSID3, myPASSWORD3);
    }
    if (--timeout < 1) // a basic authentication-timeout sorta thingy
    {
      break;
    }
  }
  if (WiFi.status() != WL_CONNECTED) 
  {
    // this is also used by handleConfig(), dont delete this line!
    m_ssidScan = WiFi.scanNetworks();
      
      Serial.println(F("WiFi connection FAILED."));
      WiFi.printDiag(Serial);
    
      if (m_ssidScan == 0)
      {
        Serial.println(F("no networks found"));
      }
      else
      {
        Serial.print(m_ssidScan);
        Serial.println(F(" networks found"));
        for (int i = 0; i < m_ssidScan; ++i)
        {   
          // Print SSID and RSSI for each network found
          Serial.print(i + 1);
          Serial.print(F(": "));
          Serial.print(WiFi.SSID(i));
          Serial.print(F(" ("));
          Serial.print(WiFi.RSSI(i));
          Serial.print(F(")"));
          Serial.println((WiFi.encryptionType(i) == ENC_TYPE_NONE)?" ":"*");
          delay(10);
        }
      }
    //starting software Access Point
    WiFi.mode(WIFI_OFF);
    delay(1);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Regador", "RegadorAndrioli");
    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(myIP);
    server.on ( "/wifi", handleConfig );
  }
  else 
  {
    Serial.println(F("WiFi connected"));    
    Serial.println(WiFi.localIP());
  }
  Serial.println(m_sHR);
  digitalWrite(HW_LEDR, false);
}

void setupSSDP()
{
  Serial.println(F("Setting up SSDP..."));
  if (WiFi.status() != WL_CONNECTED) 
  {
    Serial.println(F("ERROR: SSDP needs WiFi!"));
    //WiFi.printDiag(Serial);
  }
  else 
  {
    SSDP.setSchemaURL("description.xml");
    SSDP.setHTTPPort(80);
    SSDP.setName("Node 8266");
    SSDP.setSerialNumber(ESP.getChipId());
    SSDP.setURL("index.html");
    SSDP.setModelName("JCAM Regador 67");
    SSDP.setModelNumber("ESP8266 ESP-12F");
    SSDP.setModelURL("http://www.nosite.com");
    SSDP.setManufacturer("Joao Carlos Andrioli Machado");
    SSDP.setManufacturerURL("http://www.andrioli.ca/rlos");
    SSDP.setDeviceType("upnp:rootdevice");
    SSDP.begin();
    Serial.println(F("SSDP is up"));
    Serial.println(m_sHR);
  }
}

void setup_spiffs()
{
  uint32_t realSize = ESP.getFlashChipRealSize();
  uint32_t ideSize = ESP.getFlashChipSize();
  FlashMode_t ideMode = ESP.getFlashChipMode();

  if(ideSize == realSize) 
  {
    if (!SPIFFS.begin()) {
      Serial.println(F("Failed to mount file system"));
      return;
    }
    if (!loadConfig()) {
      Serial.println(F("Failed to load config"));
    }
  } 
  else 
  {
    Serial.println(F("Flash Chip configuration wrong!\n"));
  }
  printf(("Flash real id:   %08X\n"), ESP.getFlashChipId());
  printf(("Flash real size: %u\n"), realSize);
  printf(("Flash ide  size: %u\n"), ideSize);
  printf(("Flash ide speed: %u\n"), ESP.getFlashChipSpeed());
  printf(("Flash ide mode:  %s\n"), (ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT" : ideMode == FM_DIO ? "DIO" : ideMode == FM_DOUT ? "DOUT" : "UNKNOWN"));
  Serial.println(m_sHR);
}


String getPage()
{
  String page = F("<html lang='br'><head><meta http-equiv='refresh' content='60' name='viewport' content='width=device-width, initial-scale=1'/>"\
   "<link rel='stylesheet' href='https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap.min.css'>"\
   "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.1.1/jquery.min.js'></script>"\
   "<script src='https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/js/bootstrap.min.js'></script>"\
   "<title>Regador Wifi da Lourdes</title></head><body>"\
   "<div class='container-fluid'>"\
     "<div class='row'>"\
       "<div class='col-md-12'>"\
         "<h1>Regador Wifi</h1>"\
         "<h3>Mini estacao de controle de hidrata&ccedil;&atilde;o dos seres verdes</h3>"\
         "<ul class='nav nav-pills'>"\
           "<li class='active'>"\
             "<a href='#'> <span class='badge pull-right'>WATERLOW"\
             "</span> N&iacute;vel de &aacute;gua</a>"\
           "</li><li>"\
             "<a href='#'> <span class='badge pull-right'>UPTIME"\
             "</span> Uptime</a></li>"\
         "</ul><form action='/' method='POST'><h4>Programa&ccedil;&atilde;o di&aacute;ria: esta tabela determina daqui h&aacute; quanto tempo a irriga&ccedil;&atilde;o come&ccedil;a.</h4>"\
         "<table class='table'>"\
           "<thead><tr><th>Rel&eacute;</th><th tooltip='Intervalo em minutos entre cada ativacao'>Intervalo</th><th>Dura&ccedil;&atilde;o</th></tr></thead>"\
           "<tbody>"\
             "<tr STATE1ACTIVEINACTIVE><td>1</td><td><input type=text maxlength=4 value=SCHED1 name=sched1 size=4 />min</td><td><input type=text value=MAXDUR1 name=maxdur1 size=2 maxlength=2 />s</td></tr>"\
             "<tr STATE2ACTIVEINACTIVE><td>2</td><td><input type=text maxlength=4 value=SCHED2 name=sched2 size=4 />min</td><td><input type=text value=MAXDUR2 name=maxdur2 size=2 maxlength=2 />s</td></tr>"\
         "</tbody></table><p>Repetir este programa a cada <input type=number min=1 max=4 name=hebdo value=VALHEBDO /> dia(s).</p>"\
         "<button type='button submit' name='sched' value='1' class='btn btn-success btn-lg'>Salvar</button></form>"\
         "<h3>Acionamento manual</h3>"\
         "<div class='row'>"\
            "<div class='col-md-4'><h4 class ='text-left'>D5<span class='badge'>SPAN1ONOFF</span><form action='/' method='POST'>"\
            "<button type='button submit' name='D5' value='STATE1ONEORZERO' class='btn STATE1BTNCLASS btn-lg'>STATE1ONOFF</button></form></h4></div>"\
            "<div class='col-md-4'><h4 class ='text-left'>D6<span class='badge'>SPAN2ONOFF</span><form action='/' method='POST'>"\
            "<button type='button submit' name='D6' value='STATE2ONEORZERO' class='btn STATE2BTNCLASS btn-lg'>STATE2ONOFF</button></form></h4></div>"\
   "</div></div></div><p>N&atilde;o deixe esta p&aacute;gina aberta. Feche assim que tiver terminado.</p>"\
   "</body></html>");
  page.replace(F("UPTIME"), String(myData.uptime));
  page.replace(F("WATERLOW"), String(myData.waterlow));
  page.replace(F("STATE1ONEORZERO"), String((!myData.state1)));
  page.replace(F("STATE2ONEORZERO"), String((!myData.state2)));
  page.replace(F("STATE1ONOFF"), (!myData.state1)?m_sON:m_sOFF);
  page.replace(F("STATE2ONOFF"), (!myData.state2)?m_sON:m_sOFF);
  page.replace(F("SPAN1ONOFF"), (myData.state1)?m_sON:m_sOFF);
  page.replace(F("SPAN2ONOFF"), (myData.state2)?m_sON:m_sOFF);
  page.replace(F("STATE1BTNCLASS"), (!myData.state1)?m_sBtnSuccess:m_sBtnDanger);
  page.replace(F("STATE2BTNCLASS"), (!myData.state2)?m_sBtnSuccess:m_sBtnDanger);
  page.replace(F("STATE1ACTIVEINACTIVE"), myData.state1?m_sClassActive:"");
  page.replace(F("STATE2ACTIVEINACTIVE"), myData.state2?m_sClassActive:"");
  page.replace(F("SCHED1"), String(myData.sched1));
  page.replace(F("SCHED2"), String(myData.sched2));
  page.replace(F("MAXDUR1"), String(myData.maxdur1));
  page.replace(F("MAXDUR2"), String(myData.maxdur2));
  page.replace(F("VALHEBDO"), String(m_hebdo));
  return page;
}

void handleRoot()
{
  if ( server.hasArg(F("D5")) ) {
    handleD5();
  } else if ( server.hasArg(F("D6")) ) {
    handleD6();
  } else if ( server.hasArg(F("sched")) ) {
    handleSched();
    saveConfig();
  } 
  server.send ( 200, "text/html", getPage() );
}

void handleConfig()
{
  if ( server.hasArg(F("setnewssid")) ) 
  {
    g_nwSSID = server.arg(F("setnewssid"));
  } 
  if ( server.hasArg(("setnewpass")) ) 
  {
    g_nwPASS = server.arg(("setnewpass"));
  } 
  if ( server.hasArg(F("setnewssid")) || server.hasArg(F("setnewpass")) ) 
  {
    saveConfig();
  }
  if ( server.hasArg(F("reboot")) ) 
  {
    ESP.restart();
  } 
  String strWifiPage = F("<html lang='br'><head><meta http-equiv='refresh' content='60' name='viewport' content='width=device-width, initial-scale=1'/>"\
   "<title>Regador Wifi da Lourdes</title></head><body>"\
   "<h1>Acesso temporario! <br/>AP de configuracao</h1><form action=/wifi method=post>"\
   "<p>WiFi SSID:<input name='setnewssid' value='NWSSID' /></p>"\
   "<p>WiFi Pass:<input name='setnewpass' value='NWPASS' />"\
   "<br/><input type='submit' name='submit' value='Submit' /></p></form>"\
   "<form action=/wifi method=post><input type='submit' name='reboot' value='Reset' /></form>");
   
  if ( m_ssidScan > 0)
  {
    strWifiPage += "<pre><ul>";
    for (int i = 0; i < m_ssidScan; i++)
    {
       strWifiPage += "<li>";
       strWifiPage +=    String(i + 1);
       strWifiPage +=    ": ";
       strWifiPage +=    WiFi.SSID(i);
       strWifiPage +=    " (";
       strWifiPage +=    WiFi.RSSI(i);
       strWifiPage +=    ")";
       strWifiPage += "</li>";
    }
    strWifiPage += "<ul></pre>";
  }
  strWifiPage += ("</body></html>");
  strWifiPage.replace("NWSSID", g_nwSSID);
  strWifiPage.replace("NWPASS", g_nwPASS);
  server.send(200, "text/html", strWifiPage);
}

void handleSched()
{
  String s1,s2,d1,d2,hebdo;
  s1 = server.arg("sched1");
  s2 = server.arg("sched2");
  d1 = server.arg("maxdur1");
  d2 = server.arg("maxdur2");
  hebdo = server.arg("hebdo");
  myData.sched1 = s1.toInt();
  myData.sched2 = s2.toInt();
  myData.maxdur1 = d1.toInt();
  myData.maxdur2 = d2.toInt();
  m_hebdo = hebdo.toInt();
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

void printInfos()
{
  Serial.println( "Compiled: " __DATE__ ", " __TIME__ ", " __VERSION__);
  print_system_info(Serial);

  Serial.printf(("Free sketch space %d\n"), ESP.getFreeSketchSpace());

  Serial.print(F("wifi_get_opmode(): "));
  Serial.print(wifi_get_opmode());
  Serial.print(F(" - "));
  Serial.println(OP_MODE_NAMES[wifi_get_opmode()]);

  Serial.print(F("wifi_get_opmode_default(): "));
  Serial.print(wifi_get_opmode_default());
  Serial.print(F(" - "));
  Serial.println(OP_MODE_NAMES[wifi_get_opmode_default()]);

  Serial.print(F("wifi_get_broadcast_if(): "));
  Serial.println(wifi_get_broadcast_if());
  print_wifi_general(Serial);
  Serial.print(F("WiFi MAC Address: "));
  Serial.println(WiFi.macAddress());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("WiFi connected"));    
    Serial.println(WiFi.localIP());
  }
  else {
    Serial.print(F("WiFi status: "));    
    Serial.println(WiFi.status());
  }
  Serial.println(m_sHR);  
}

void blinkLeds(int a)
{
  for (int j=0; j <= a; j++)
  {
    digitalWrite(HW_LEDG, !b);
    digitalWrite(HW_LEDR, b);
    delay(250);
    b = !b;
  }
  digitalWrite(HW_LEDG, LOW);
  digitalWrite(HW_LEDR, LOW);
} //blinkLeds


/*
 * © Francesco Potortì 2013 - GPLv3
 *
 * Send an HTTP packet and wait for the response, return the Unix time
 *
long int webUnixTime ()
{
  unsigned long time = 0;
  WiFiClient client;

  // Just choose any reasonably busy web server, the load is really low
  if (client.connect("172.217.13.206", 80))
  {
      // Make an HTTP 1.1 request which is missing a Host: header
      // compliant servers are required to answer with an error that includes
      // a Date: header.
      client.print(F("GET / HTTP/1.1 \r\n\r\n"));

      char buf[5];      // temporary buffer for characters
      int timeout = millis() + 5000;
      while ( client.available() == 0)
      {
        if ( timeout - millis() < 0)
        {
          Serial.println("time timeout!");
          client.stop();
          return 0;
        }
      }
      if (client.find((char *)"\r\nDate: ") // look for Date: header
          && client.readBytes(buf, 5) == 5) // discard
      {
        unsigned day = client.parseInt();    // day
        client.readBytes(buf, 1);    // discard
        client.readBytes(buf, 3);    // month
        int year = client.parseInt();    // year
        byte hour = client.parseInt();   // hour
        byte minute = client.parseInt(); // minute
        byte second = client.parseInt(); // second
  
        int daysInPrevMonths;
        switch (buf[0])
        {
          case 'F': daysInPrevMonths =  31; break; // Feb
          case 'S': daysInPrevMonths = 243; break; // Sep
          case 'O': daysInPrevMonths = 273; break; // Oct
          case 'N': daysInPrevMonths = 304; break; // Nov
          case 'D': daysInPrevMonths = 334; break; // Dec
          default:
            if (buf[0] == 'J' && buf[1] == 'a')
              daysInPrevMonths = 0;   // Jan
            else if (buf[0] == 'A' && buf[1] == 'p')
              daysInPrevMonths = 90;    // Apr
            else switch (buf[2])
            {
              case 'r': daysInPrevMonths =  59; break; // Mar
              case 'y': daysInPrevMonths = 120; break; // May
              case 'n': daysInPrevMonths = 151; break; // Jun
              case 'l': daysInPrevMonths = 181; break; // Jul
              default: // add a default label here to avoid compiler warning
              case 'g': daysInPrevMonths = 212; break; // Aug
            }
          }

      // This code will not work after February 2100
      // because it does not account for 2100 not being a leap year and because
      // we use the day variable as accumulator, which would overflow in 2149
      day += (year - 1970) * 365; // days from 1970 to the whole past year
      day += (year - 1969) >> 2;  // plus one day per leap year 
      day += daysInPrevMonths;  // plus days for previous months this year
      if (daysInPrevMonths >= 59  // if we are past February
          && ((year & 3) == 0)) // and this is a leap year
        day += 1;     // add one day
      // Remove today, add hours, minutes and seconds this month
      time = (((day-1ul) * 24 + hour) * 60 + minute) * 60 + second;
    }
    Serial.println(F("Got internet time :-)"));
  }
  else
    Serial.println("Could not connect to g.com");
  delay(10);
  client.flush();
  client.stop();

  Serial.println(m_sHR);  
  return time;
} // webUnixTime
*/

void setup() 
{
  WiFi.mode(WIFI_OFF);
  yield();
  pinMode(HW_LEDG, OUTPUT);
  pinMode(HW_LEDR, OUTPUT);
  pinMode(HW_WATER, INPUT_PULLUP);
  pinMode(HW_RELAY1, OUTPUT);
  pinMode(HW_RELAY2, OUTPUT);
  
  blinkLeds(5);
  //
  // Print preamble
  //
  Serial.begin(74880);
  delay(100);
  Serial.println();
  Serial.println(F("========================"));  
  Serial.println(F("  REGADOR WIFI ESP-12E  "));  
  Serial.println(F("========================"));  
  delay(100);
  Serial.println(F("Warning! Always query the controller node before attempting to program it!"));  
  delay(100);
  Serial.println(m_sHR); 
  
  Serial.print(F("system_get_time(): "));
  Serial.println(system_get_time());

  printInfos();
  //prepare and configure SPIFFS
  setup_spiffs();
  blinkLeds(5);
  
  // setting up WLAN related stuff 

  // set_event_handler_cb_stream(Serial);
  wifi_set_event_handler_cb(wifi_event_handler_cb);
  setup_wifi();
  yield();
  blinkLeds(4);
  
  setupSSDP();
  yield();
  blinkLeds(3);
  
  yield();

  //setTime( webUnixTime( timeClient) );
  //setSyncProvider(webUnixTime);  // set the external time provider
  //setSyncInterval(24*60*60);         // set the number of seconds between re-sync

  server.on("/description.xml", HTTP_GET, [](){
    Serial.println(F("HTTP server got request for desc.xml"));
    SSDP.schema(server.client());
  });
  server.on ( "/", handleRoot );
  server.on ( "", handleRoot );
  server.begin();  
  blinkLeds(2);
  Serial.println ( F("HTTP server started") );
  
  Serial.print(F("system_get_time(): "));
  Serial.println(system_get_time());


  if ( wifi_get_opmode() != 2 && ( WiFi.status() == WL_CONNECTED || m_gotIP)) 
    bot.sendMessage(g_tgCHAT, "Regador inicializado.", "HTML");
}

void loop() 
{
  yield();
  server.handleClient();
  yield();

  if ( millis() - t > 10000 )
  {
    if ( wifi_get_opmode() == 2 ) //    if (WiFi.status() != WL_CONNECTED) 
      digitalWrite(HW_LEDR, b);
    else
      digitalWrite(HW_LEDG, b);
    
    myData.waterlow = !digitalRead(HW_WATER);
    myData.uptime = (float)millis() / (float)60000;
    
    b = !b;
    t = millis();
  }
  
  while (Serial.available())
  {
    String s1 = Serial.readStringUntil('\n');
    Serial.println(F("CAREFUL, end of line is only NL and no CR!!!"));
    Serial.print(F("You typed:"));
    Serial.println(s1);
    if (s1.indexOf("setnewssid ")>=0)
    {
      s1 = s1.substring(s1.indexOf(" ")+1);
      g_nwSSID = s1.substring(0, s1.length());
      Serial.println(("new ssid is now [") + g_nwSSID + "]");
    }
    else if (s1.indexOf("setnewpass ")>=0)
    {
      s1 = s1.substring(s1.indexOf(" ")+1);
      g_nwPASS = s1.substring(0, s1.length());
      Serial.println(("new pass is now [") + g_nwPASS + "]");
    }
    else if (s1.indexOf("setnewpass ")>=0)
    {
      s1 = s1.substring(s1.indexOf(" ")+1);
      g_tgCHAT = s1.substring(0, s1.length());
      Serial.println(("new pass is now [") + g_tgCHAT + "]");
    }
    else if (s1.indexOf("save")>=0)
    {
      saveConfig();
    }
    else if (s1.indexOf("reboot please")>=0)
    {
      ESP.restart();
    }
    else if (s1.indexOf("debug")>=0)
    {
      printInfos();
    }      
    else if ((s1.indexOf("setnewpass")!=0) && (s1.indexOf("setnewssid")!=0) && (s1.indexOf("setnewchat")!=0))
    {
      Serial.println(F("** Serial interface expects:\n\r"\
        "** 0 - setnewssid: set a new SSID for the module\n\r"\
        "** 1 - setnewpass: set a new PSK key for the nw\n\r"\
        "** 1 - setnewchat: set a new chat destinataire\n\r"\
        "** 3 - save : save the configuration into a file on the flash"));
    }
  }

  if ( wifi_get_opmode() == 2 ) // AP mode, temporary only. Do nothing else until a real Wifi is cfg'd
  {
    return;
  }

  //try to convert millis() to now()
  /*if (myData.sched1 > 0 && timeStatus() == timeSet)
  {
    // if the next scheduled activation is in the past, we now we can convert
    // note that is now==0, we have no time
    if (myData.sched1 < now())
    {
      myData.sched1 += now();
      myData.sched2 += now();
    }
  }*/

  // do not have therm on esp systems
  //if ( readTemperature() < myData.temp_thres )
  {
    if ( millis()/60000 >= myData.sched1  && myData.sched1 > 0  )
    {
      if (myData.state1 = false) Serial.println(F("Activating plug1"));
      myData.state1 = true;
      if (!activation_notified)
      { 
        bot.sendMessage(g_tgCHAT, "Acionando regador, bomba 1.", "HTML");
        activation_notified = true;
      }
    }
    if ( millis()/60000 >= myData.sched2  && myData.sched2 > 0  )
    {
      if (myData.state2 = false) Serial.println(F("Activating plug2"));
      myData.state2 = true;
      if (!activation_notified)
      { 
        bot.sendMessage(g_tgCHAT, "Acionando regador, bomba 2.", "HTML");
        activation_notified = true;
      }
    }
  }
    
  // switch relays off after max_duration, & sendout new status
  // WARNING: ===> calculate in seconds coz duration is in secs
  if ( myData.sched1 > 0 && millis()/1000 > (myData.sched1*60)+myData.maxdur1 )
  { 
    bot.sendMessage(g_tgCHAT, "Desligando regador, bomba 1.", "HTML");
    myData.state1 = false;
    //automatically schedule relay1 to tomorrow
    myData.sched1 += 1440*m_hebdo;
    activation_notified = false;
  }
  if ( myData.sched2 > 0 && millis()/1000 > (myData.sched2*60)+myData.maxdur2 )
  { 
    bot.sendMessage(g_tgCHAT, "Desligando regador, bomba 2.", "HTML");
    myData.state2 = false;
    //automatically schedule relay2 to tomorrow
    myData.sched2 += 1440*m_hebdo; 
    activation_notified = false;
  }


  // just to avoid calling digitalWrite all the time. I think this is screwing me
  if ( bRealState1 != myData.state1 )
  {
    digitalWrite(HW_RELAY1, myData.state1); 
    bRealState1 != myData.state1 ;
  }
  if ( bRealState2 != myData.state2 )
  {
    digitalWrite(HW_RELAY2, myData.state2); 
    bRealState2 = myData.state2 ;
  }


  if ( WiFi.status() == WL_CONNECTED || m_gotIP) 
  {
    if (millis() > Bot_lasttime + Bot_mtbs)  
    {
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      while(numNewMessages) 
      {
        for (int i=0; i<numNewMessages; i++) 
        {
          const telegramMessage& tm = bot.messages[i];
          
          if ( tm.chat_id.length() < 5 || tm.text.length() < 2 ) 
          {
            Serial.println("ERROR! Ignoring empty telegram message");
            Serial.println("ERROR! LastMessageReceived " + String(bot.last_message_received));
            numNewMessages = bot.getUpdates(bot.last_message_received + 1);
            continue; 
          }
          Serial.print("chatter " + tm.chat_id);
          Serial.println(", text " + tm.text);
          blinkLeds(1);
          if (tm.chat_id.length()>5 && tm.chat_id!=g_tgCHAT)
            bot.sendMessage("60001082", "Usuario estranho conversando com o regador:<pre>" + 
            tm.chat_id + 
            "</pre>" + tm.from_name, "HTML");
          else if (tm.text=="/help")
            bot.sendMessage(g_tgCHAT, "Visite <a href=\"http://" + WiFi.localIP().toString() + "\">este link</a> para manipular o regador.", "HTML");
          else if (tm.text.indexOf("IP") >= 0)
            bot.sendMessage(g_tgCHAT, "Meu endereco local e <pre>" + WiFi.localIP().toString() + "</pre>", "HTML");
          else if (tm.text.indexOf("uptime") >= 0)
          {
            bot.sendMessage(g_tgCHAT, "Estou funcionando já fazem " + String(millis()/60000) + "min.");
          }
          else if (tm.text.indexOf("ativar regador") >= 0)
          {
            myData.sched1 = myData.uptime;
            myData.sched2 = myData.uptime + 1;
          }
          else if (tm.text.indexOf("stop") >= 0 || tm.text.indexOf("desativar") >= 0)
          {
            myData.sched1 = 0;
            myData.sched2 = 0;
          }
          else
          {
            bot.sendMessage(tm.chat_id, "Vc escreveu <pre>" + tm.text + "</pre> mas ainda nao estou programado para fazer nada com esta informacao.", "HTML");
          }
        }
        numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      }
      Bot_lasttime = millis();
    }
  }
}
