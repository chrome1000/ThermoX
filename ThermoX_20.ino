/**************************************************************************** 
 *  ThermoX v0.20.0 HVAC Thermostat
 *  
 *  Compares ambient and desired temperatures, and runs HVAC as needed.  
 *  Hysteresis levels for both Summer and Winter are independently adjustable. 
 *  Sensor can be corrected +/- 5 degrees. All user preferences are saved, and 
 *  reloaded on startup.
 *  
 *  HOME setting can be triggered in IFTTT by your cell phone location, triggering  
 *  an action with Webhooks. Webhooks parameters are as follows:
 *       URL: http://blynk-cloud.com:8080/YOUR_TOKEN/pin/V31
 *       Method: PUT
 *       Content Type: application/json
 *       Body: ["1"]    
 *  Make an identical IFTTT recipe for AWAY but use ["0"] for the body parmeter.
 *  
 *  Color coded DESIRED TEMPERATURE widget: red/blue/green for heat/cool/off
 *  modes, respectively. 
 *    
 *  PERCEIVED TEMPERATURE mode augments actual temperature when Summer humidity is high.
 *  
 *  PULSE mode runs system for 15 minutes.
 *  
 *  New minimum and maximum temperature settings override "away" mode (my plants were 
 *  dying). Now, even in AWAY mode, HVAC will come on if min/max limits are exceded.
 *  
 *  Added native Alexa control (HUE emulation). 
 *    -   "Turn ON" deactivates system halt, and runs pulse mode. 
 *    -   "Turn OFF" cancels  a pulse, or activates system halt if no pulse was running.     
 *    -   "Turn UP/DOWN" chages temperature by 2 degrees.
 *    -   "Set to (number)" sets a specific new desired temperature.
 * 
 *  Automatically reconnects to last working wifi. If unavailable, it creates an access 
 *  point ("ThermoX") to which you can connect and input locl wifi credentials. *  
******************************************************************************/

#include <ESP8266WiFi.h>        //https://github.com/esp8266/Arduino
#include <ESP8266WebServer.h>
#include <DNSServer.h>

#include <BlynkSimpleEsp8266.h> //https://github.com/blynkkk/blynk-library
#include <WiFiManager.h>        //https://github.com/tzapu/WiFiManager
#include "DHT.h"                //https://github.com/adafruit/DHT-sensor-library
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <Espalexa.h>           //https://github.com/Aircoookie/Espalexa

#define UpdateFrequency 8000    //How often a new temperature will be read
#define MenuTimeOut 10000       //Menu timeout from inactivity
#define LongPress 650           //How long SETTINGS button needs to be pressed to enter menu
#define RelayPin 2
#define OFF 0                   // These just make the code easier to read
#define ON 1

//WiFi and Blynk connection variables
String myHostname = "ThermoX";
char auth[] = "YOUR_BLYNK_TOKEN"; // Blynk token "YourAuthToken"

//Set up as a native Alexa device (Hue emulation)
char Device1[] = "ThermoX";     // ON/OFF switch name in the Alexa app
EspalexaDevice* espalexaPointer;
Espalexa espalexa;

// Blynk color palette
const String BLYNK_BLUE =    "#04C0F8";
const String BLYNK_RED   =   "#D3435C";
const String BLYNK_GREEN  =  "#23C48E";

DHT dht(0,DHT11); //Initialize the sensor. Use pin 0. Sensor type is DHT11.

// Timer for temperature updates
BlynkTimer timer;

//Thermostat variables
int TempDes = 70;             //Desired temperature setting
int PreviousTempDes;
int TempAct = 70;             //Actual temperature, as measured by the DHT11 sensor
int BadRead = 0;              //Counts consecutive failed readings of the DHT11 sensor
float LastRead = 70;          // Previous temperature reading
int Humidity = 50; 
int TempMin = 58;             // Minimum allowable temperature, even if in "away" mode
int TempMax = 90;             // Maximum allowable temperature, even if in "away" mode

// Preference variables
int Hysteresis_W = 2;         //Summer and Winter hysteresis levels
int Hysteresis_S = 2;
int TempCorrection = 0;       //Used to adjust readings, if the sensor needs calibration
boolean UsePerceivedTemp = false; // Use humidity-adjusted perceived temperature, instead of actual temperature
long PulseTime = 15 * 60 * 1000; // Amount of time for a "pulse" manual run of the system (15 minutes)

// Current condition variables
boolean Winter = true; 
boolean Home = true;
boolean ManualRun = false;    // used to run fan, overriding thermostat algorithm
boolean ManualStop = false;   // used to stop fan, overriding thermostat algorithm
int MenuItem = 0;             // Settings menu selection variable
boolean ButtonPressed = false;// Settings button state
boolean LongHold = false;     // Flag showoing a long hold detected on the SETTINGS button
int ButtonTimer;              // Timer for detecting long press of Settings button
String Response = "";         // Text output to SETTINGS value widget
boolean FanState = OFF;       // Is the fan on or off?
int MenuTimer;                // Timer for resetting SETTINGS menu after a timeout has elapsed


void setup() {
  
  // Create an access point if no wifi credentials are stored
  WiFi.hostname(myHostname);
  WiFiManager wifi;
  wifi.autoConnect("ThermoX"); 
  Blynk.config(auth);
  
  dht.begin(); //Start temperature sensor and wait for initialization
  delay(1500);

  //Initialize the fan relay. Mine is "off" when the relay is set HIGH.
  pinMode(RelayPin,OUTPUT); 
  digitalWrite(RelayPin,HIGH);
 
  Serial.begin(115200);
  
  //Load any saved settings from the EEPROM
  EEPROM.begin(20);  
  GetPresets();
  PreviousTempDes = TempDes; 
  
  MenuReset();

  ArduinoOTA.begin();

  // Espalexa initialization. Parameters: (device name, callback function, device type, initial value)
  espalexaPointer = new EspalexaDevice(Device1, AlexaCommands, EspalexaDeviceType::dimmable, TempDes * 2.55); 
  espalexa.addDevice(espalexaPointer);
  espalexa.begin();

  timer.setInterval(UpdateFrequency, TempUpdate); // Update temp reading and relay state
  timer.setInterval(30000L, OtherUpdates);        // Refreshes non-urgent dashboard info
}


void loop() {
  Blynk.run();
  timer.run();
  ArduinoOTA.handle();
  espalexa.loop();
}


//*********************** Thermostat Functions **********************************

// This is the decision algorithm for turning the HVAC on and off
void TempUpdate (){
  float ReadF = dht.readTemperature(true); //Get a new reading from the temp sensor
    
  if (isnan(ReadF)) {
    BadRead++;
    return;
  }

  // Use perceived temperature instead of actual temperature for Summer cooling
  if(UsePerceivedTemp == true && !Winter && ReadF > 70){
    // Because perceived temp swings can be large, augment by only a fraction of
    // a degree per read. Changes are slowed, and more samples inform the average.
    if(ReadF > LastRead + 0.5){
      ReadF = LastRead + 0.5;  
    }
    else if(ReadF < LastRead - 0.5){
      ReadF = LastRead - 0.5;
    }
    // Simplified "feels like" temperature formula
    ReadF = ((Humidity * .02 * (ReadF - 70)) + ReadF);
  }

  //To compensate for the DH11's inaccuracy, the temperature is averaged
  //withprevious read, and any change is limited to 1 degree at a time. 
  int TempAvg = int((ReadF + LastRead + (2 * TempCorrection))/2);
  if (TempAvg > TempAct){
    TempAct += 1;
  }
  else if (TempAvg < TempAct){
    TempAct -= 1;
  }

  LastRead = ReadF;
  BadRead = 0;        // Reset counter for failed sensor reads
  
  Blynk.virtualWrite(V0,TempAct); //Report the corrected temperature in app

  // Decision algorithm for running HVAC
  if (!ManualRun && !ManualStop){   // Make sure it's not in one of the manual modes
    // If I'm home, run the algorithm
    if (Home){
      if (Winter){
        //If I'm home, it's Winter, and the temp is too low, turn the relay ON
        if (TempAct < TempDes){
          Fan(ON);
        }
        //Turn it off when the space is heated to the desired temp + a few degrees
        else if (TempAct >= (TempDes + Hysteresis_W)) {
          Fan(OFF);
        }
      }
      else if (!Winter){
        //If I'm home, it's Summer, and the temp is too high, turn the relay ON
        if (TempAct > TempDes){
          Fan(ON);
        }
        //Turn it off when the space is cooled to the desired temp - a few degrees
        else if (TempAct <= (TempDes - Hysteresis_S)){
          Fan(OFF);
        }
     }
    }
    // If I'm not home...
    else {
      // Turn on the HVAC if the temperature outside of seasonal the minimum / maximum limits
      if((Winter && TempAct < TempMin) || (!Winter && TempAct > TempMax)){
        Fan(ON);
      }
      // Otherwise, turn it off
      else{
        Fan(OFF);
      }   
    }
  }
}


// Turn the HVAC ON or OFF
void Fan(boolean RunFan){
  FanState = RunFan;

  // Set the proper color for the Desired Temp gauge and ON/OFF LED
  //(red = heating, blue = cooling, fan off = normal widget color
  if (Winter && FanState){
      Blynk.setProperty(V0, "color", BLYNK_RED);
    }
    else if (!Winter && FanState){
      Blynk.setProperty(V0, "color", BLYNK_BLUE);
    }
    else{
      // Return widgets to their "off" state color, depending on theme
        Blynk.setProperty(V0, "color", BLYNK_GREEN);      
    }
    
  digitalWrite(RelayPin,!FanState); // Relay turns fan on with LOW input, off with HIGH
}


// Ends manual pulse mode
void KillManual(){
  Fan(OFF);
  ManualRun = false;
}


//Temperature slider. Make the desired temperature gauge in Blynk reflect slider changes.
BLYNK_WRITE(V3){
  TempDes = param.asInt();
  Blynk.virtualWrite(V1,TempDes);
  ManualStop = false;      //New temperature setting ends any manual stop
  if(espalexaPointer != nullptr){     //Update espalexa "brightness" value
    espalexaPointer->setPercent(TempDes); 
  }
}

// Updates dashboard information on the Blynk app
void OtherUpdates(){
  Blynk.virtualWrite(V29,Home * 1023); // Update "home" LED on dashboard
  Blynk.virtualWrite(V1,TempDes);      //Update desired temp on the dashboard
   
   // Notify when the temperature sensor fails repeatedly, and turn off the fan.
   if(MenuItem == 0 && !ButtonPressed){
     if (BadRead > 10){
       Blynk.virtualWrite(V10, String("<<< SENSOR MALFUNCTION >>>"));
       BadRead = 0;
       if (!ManualRun){ //Manual mode supersedes a malfunction condition
        Fan(OFF);
       }
     }
     // Clear notification when sensor reads correctly again
     else{
      MenuReset();
     }
   }
   
   if (TempDes != PreviousTempDes){   //update the EEPROM if desired temperature had changed.
    EEPROM.write(3,TempDes);
    EEPROM.commit();
    PreviousTempDes = TempDes;  
   }

  // To stabilize perceived temperature calculation, only update humidity readings between fan cycles
  if(FanState == OFF){
    float ReadH = dht.readHumidity();          // Read humidity (percent)

    // Only update humidity if it's a good read from the sensor. To mitigate any
    // instability, average with previous reading, change by only 1% per reading
    if(!(isnan(ReadH))){
      int HumidityAvg = (ReadH + Humidity) / 2;
      if (HumidityAvg > Humidity){
        Humidity += 1;
      }
      if (HumidityAvg < Humidity){
        Humidity -=1;
      }
    }
     Blynk.virtualWrite(V2, Humidity);
  }   
}


//************************ External Changes (Alexa, IFTTT) ************************************
// Alexa native device (shows up in Alexa app as a Hue device)
void AlexaCommands(EspalexaDevice* espalexaPointer) { 
  if(espalexaPointer == nullptr) return;

  //Retrieve numeric value, and show in Blynk settings tab
  int AlexaPercent = espalexaPointer->getPercent();
  Response = "Alexa temp: ";
  Response += AlexaPercent;
  Blynk.virtualWrite(V10,Response);
  MenuTimer = timer.setTimeout(MenuTimeOut, MenuReset);
  
  // "Alexa, Turn OFF" ends manual run, or applies Manual stop if not running
  if(AlexaPercent == OFF){
    Fan(OFF);
    if(ManualRun){
      ManualRun = false;
    }
    else{
      ManualStop = true;
    }
  } 
  // "Alexa, turn ON," set level (temperature), or augment
  else{
    // If the fan is already ON, use imcomming level for temperature setting
    if(FanState == ON){
      //"Alexa, turn UP..." triggers an unusually big change. Incremnent 2 degrees.
      if(AlexaPercent > TempDes + 10){   
        TempDes += 2;
      }
      //"Alexa, turn DOWN..." triggers an unusually big change. Decrement 2 degrees.
      else if(AlexaPercent < TempDes - 10){   
        TempDes -= 2;
      }
      //"Alexa, set ThermoX to ##" triggers a reasonable change. Use as desired temperature.
      else if(AlexaPercent >= TempMin && AlexaPercent <= TempMax){
        TempDes = AlexaPercent;
      }  
      Blynk.virtualWrite(V1, TempDes);
      Blynk.virtualWrite(V3, TempDes);
      if(espalexaPointer != nullptr){     //Update espalexa "brightness" value
        espalexaPointer->setPercent(TempDes); 
      }  
    }
    // Otherwise, it was a "Turn ON" command, so run a pulse cycle
    else{
      ManualRun = true;
      Fan(ON);
      timer.setTimeout(PulseTime, KillManual);
    }
  }
}

//Get location (home or away) from the IFTTT iOS location and Maker channels
BLYNK_WRITE(V31)
{   
  Home = param.asInt(); 
  if (Home){ //Turn the HOME LED widget on or off
    Blynk.virtualWrite(V29,1023);
  }
  else Blynk.virtualWrite(V29,0);
}


//************************** Settings Menu Functions *******************************

// Dashboard SETTINGS button. Press-and-hold to enter menu. Short press for next item.
BLYNK_WRITE(V4) {    
  // When the SETTINGS button is pressed, start a timer to check for a long press
  if(param.asInt()){
    ButtonTimer = timer.setTimeout(750, LongHoldDetect);
    ButtonPressed = true;
  }
   
  // Button has been released
  else {
    timer.deleteTimer(ButtonTimer);   // Kill current long button hold detection
    ButtonPressed = false;        // Reset the button press flag
      
    // If the long hold function wasn't just called, it's a short press. Avance the menu.
    if (!LongHold && MenuItem != 0){    
      NextMenuItem(); // Advance to next menu item
    }
    // Reset the long press flag
    LongHold = false;
  }
}

// Checks for long press condition on SETTINGS button
void LongHoldDetect(){
  // If the button is still depressed, it's a long hold
  if (ButtonPressed && LongHold == false){  
    // Enter or exit the SETTINGS menu, if it was a long press 
    LongHold = true;      // Flag prevents repeated tripping of long hold
    if (MenuItem == 0){
      MenuTimer = timer.setTimeout(MenuTimeOut, MenuReset);
      NextMenuItem(); // Enter the SETTINGS menu    
    }
    else{
      MenuReset(); // Exit the SETTINGS menu
    }
  }
}


//Cycles through the Settings Menu in the Labeled Value widget
void NextMenuItem(){
  timer.restartTimer(MenuTimer);
   
  MenuItem += 1;
  if (MenuItem > 8){
    MenuItem = 1;
  }
    
  switch(MenuItem){
      case 1:
        if (ManualRun){
          Response = "CANCEL PULSE?";
        }
        else{
          Response = "15 MIN PULSE?";
        }
        break;

      case 2:
        if (UsePerceivedTemp){
          Response = "USE ACTUAL TEMP?";
        }
        else Response = "USE PERCEIVED TEMP?";
        break;

      case 3:
        if (ManualStop){
          Response = "END SYSTEM HALT?";
        }
        else{
          Response = "HALT SYSTEM?";
        }
        break;
        
     case 4:
      if (Home){
        Response = "LOCATION: HOME";
      }
      else Response = "LOCATION: AWAY";
      break;


    case 5:
      if (Winter){
        Response = "MODE : WINTER";
      }
      else Response = "MODE : SUMMER";
      break;

    case 6:
      if (Winter){
        Response = "HYSTERESIS: ";
        Response +=  Hysteresis_W;
        Response += " DEG";   
      }
      else{
        Response = "HYSTERESIS: ";
        Response += Hysteresis_S;
        Response += " DEG";
      }
      break;

    case 7:
      Response = "TEMP CORRECTION: ";
      Response += TempCorrection;
      Response += " DEGREES";
      break;

    case 8:
      if(Winter){
        if(TempMin < 50 || TempMin > 90){
          Response = "SET MINIMUM TEMP?";
        }
        else{
          Response = "MINIMUM TEMP: ";
          Response += TempMin;
        }
      }
      else{
        if(TempMin < 50 || TempMin > 90){
          Response = "SET MAXIMUM TEMP?";
        }
        else{
          Response = "MAXIMUM TEMP: ";
          Response += TempMax;
        }
      }
      break;
  }
  Blynk.virtualWrite(V10,Response);
}


//Dashboard MODIFY button. Executes change of selected menu item 
BLYNK_WRITE(V5){   
  if (MenuItem > 0 && param.asInt()){ 
    timer.restartTimer(MenuTimer);
       
    switch(MenuItem){

      //Forced 15 minute run
      case 1:
        if (ManualRun){
          ManualRun = false;
          Response = "15 MIN PULSE?";
        }
        else{
          ManualRun = true;
          ManualStop = false;
          Fan(ON);
          Response = "PULSE: ON";
          timer.setTimeout(PulseTime, KillManual);
        }   
        break;

      //User perceived temperature instead of actual
      case 2:
        if (UsePerceivedTemp){
          Response = "ACTUAL TEMP MODE";
          UsePerceivedTemp = false;
          EEPROM.write(5,0);
        }
        else {
          Response = "PERCEIVED TEMP MODE";
          UsePerceivedTemp = true;
          EEPROM.write(5,1);
        }
        if(UsePerceivedTemp){
          Blynk.setProperty(V0, "label", "             Perceived Temperature");
        }
        else{
          Blynk.setProperty(V0, "label", "               Actual Temperature");
        } 
        break; 

      //Turn system off
      case 3:
        if (ManualStop){
          ManualStop = false;
          Response = "HALT SYSTEM?";
        }
        else {
          ManualStop = true;
          ManualRun = false;
          Fan(0);
          Response = "SYSTEM HALTED";
        }
        break;

       //Change location manually
      case 4:
        if (Home){
          Home = false;
          Response = "LOCATION : AWAY";
        }
        else {
          Home = true;
          Response = "LOCATION : HOME";
        }
        break;
        
      //Change season
      case 5:
        if (Winter){
          Response = "MODE : SUMMER";
          Winter = false;
          EEPROM.write(4,0);
        }
        else {
          Response = "MODE : WINTER";
          Winter = true;
          EEPROM.write(4,1);
        } 
        break;
        
      //Change hysteresis level of currently selected season
      case 6:
        if (Winter){
          Hysteresis_W += 1;
          if (Hysteresis_W > 6){
            Hysteresis_W = 1;
          }
          EEPROM.write(1,(Hysteresis_W));
          Response = "WINTER HYSTERESIS: ";
          Response += Hysteresis_W;
          Response += " DEG";
        }
        else{
          Hysteresis_S += 1;
          if (Hysteresis_S > 6){
            Hysteresis_S = 1;
          }
          EEPROM.write(2,(Hysteresis_S));
          Response = "SUMMER HYSTERESIS: ";
          Response += Hysteresis_S;
          Response += " DEG";
          }
        break;

      // Correct faulty DHT11 readings
      case 7:
        TempCorrection +=1;
        if (TempCorrection > 5){
          TempCorrection = -5;
        }
        EEPROM.write(0, TempCorrection);
        Response = "TEMP CORRECTION: ";
        Response += TempCorrection;
        Response += " DEG";
        break;

      //Change minimum Winter temperature or maximum Summer temperature
      case 8:
        if(Winter){       // Winter minimum temperature
          TempMin += 2;
          if(TempMin > 68){
            TempMin = 58;
          }
          Response = "MINIMUM TEMP: ";
          Response += TempMin;
          EEPROM.write(7,(TempMin));
        }
        else{            // Summer maximum temperature
          TempMin += 2;
          if(TempMax > 90){
            TempMax = 78;
          }
          Response = "MAXIMUM TEMP: ";
          Response += TempMax;
          EEPROM.write(8,(TempMax));
       }
    }
    EEPROM.commit();
    Blynk.virtualWrite(V10, Response);
  }
}

// Reset the Menu at startup or after timing out from inactivity
void MenuReset(){
  MenuItem = 0;
  Blynk.virtualWrite(V10, String("HOLD 2 SEC FOR MENU"));
}


//**************************** Miscellaneous *********************************
//Retrieves saved values from EEPROM
void GetPresets(){
  TempCorrection = EEPROM.read(0);
  if (!(TempCorrection >= -5) && !(TempCorrection <= 5)){
    TempCorrection = 0;
    EEPROM.write(0, 0);
  }

  UsePerceivedTemp = EEPROM.read(5);
  if(UsePerceivedTemp){
    Blynk.setProperty(V0, "label", "             Perceived Temperature");
  }
  else{
    Blynk.setProperty(V0, "label", "               Actual Temperature");
  }

  Winter = EEPROM.read(4);
  Hysteresis_W = EEPROM.read(1);
  Hysteresis_S = EEPROM.read(2);

  if (!(Hysteresis_W >= 1) && !(Hysteresis_W <= 6)){
      Hysteresis_W = 2;
      EEPROM.write(1, Hysteresis_W);
  }
  if (!(Hysteresis_W >= 1) && !(Hysteresis_W <= 6)){
      Hysteresis_S = 2;
      EEPROM.write(2, Hysteresis_S);
  }
  
  TempDes = EEPROM.read(3);
  if (!(TempDes >= 50) && !(TempDes <= 90)){
    TempDes = 70;
    EEPROM.write(3, 70);
  }
  if(espalexaPointer != nullptr){ 
    espalexaPointer->setPercent(TempDes); 
  }

  TempMin = EEPROM.read(7);
  if(!(TempMin >= 50 && TempMin <= 90)){
    TempMin = 58;
    EEPROM.write(7, TempMin);
  }
  TempMax = EEPROM.read(8);
  if(!(TempMax >= 50 && TempMax <= 90)){
    TempMax = 90;
    EEPROM.write(8, TempMax);
  }
  EEPROM.commit();
}
