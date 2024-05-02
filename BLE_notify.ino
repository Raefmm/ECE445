/*
  Based on Neil Kolban example for IDF: https://github.com/nkolban/esp32-snippets/blob/master/cpp_utils/tests/BLE%20Tests/SampleNotify.cpp
  Ported to Arduino ESP32 by Evandro Copercini
  updated by chegewara and MoThunderz
*/
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <SparkFun_Qwiic_Rfid.h>
#include <Wire.h>
#include <map>
#include "Adafruit_LTR329_LTR303.h"
#include <SparkFun_I2C_Mux_Arduino_Library.h>
#include <Preferences.h>
#include <deque>

#define NUMBER_OF_SENSORS 1

#define I2C_SDA 6
#define I2C_SCL 5

// save to flash
Preferences preferences;

// timing

hw_timer_t *Timer0_Cfg[NUMBER_OF_SENSORS];

// Mux variables
QWIICMUX myMuxRFID;
QWIICMUX myMuxLight;


// Light Sensors variables
#define LightSensor_ADDR 0x29

#define LightThreshold 10

Adafruit_LTR329 ltr[NUMBER_OF_SENSORS];


// RFID variables
#define RFID_ADDR 0x7D

#define MAX_RFID_STORAGE 20
Qwiic_Rfid * myRfids[NUMBER_OF_SENSORS];

String allTags[MAX_RFID_STORAGE]; 
//float allTimes[MAX_RFID_STORAGE];
int serialInput; 

int unassigned_tags = 0;

// Indication Variables

#define LED1_BUILTIN 9
#define LED2_BUILTIN 10
#define LED3_BUILTIN 11
#define LEDG_BUILTIN 12

int LED_ADDR[4] = {LED1_BUILTIN, LED2_BUILTIN, LED3_BUILTIN, LEDG_BUILTIN};

bool leds[4]; // the 4th is green
//int activeTags = 0;

// Bluetooth variables

BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
BLECharacteristic* pCharacteristic_2 = NULL;
BLEDescriptor *pDescr;
BLE2902 *pBLE2902;

bool deviceConnected = false;
bool oldDeviceConnected = false;
int itemStatus;
std::string value;

// my bluetooth variables
std::deque<std::string> mydeque;

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

#define SERVICE_UUID        "481ab1c4-c161-4bfb-a018-15ab7f4f0a2c"
#define CHAR1_UUID          "967626f8-44ca-4971-b68d-2b45b13cba20" //MCU to APP
#define CHAR2_UUID          "517a24ce-9060-4a37-99ec-f1d3b4ca6e68" //APP to MCU

//Item class and map
class item{
  public:
    std::string RFID;
    std::string name;
    int AssignedLocation; //location is one of the following: -1 (unassigned), 1, 2, 3
    int LastKnownLocation;
    //float LastKnownTime;
    bool inside_bag = false;

    item(){
      this->RFID = "";
      this->name = "";
      this->AssignedLocation = -1;
      this->LastKnownLocation = -1;
      //this->LastKnownTime = -1;
    }

    item(std::string ID, std::string description, int Location){
      this->RFID = ID;
      this->name = description;
      this->AssignedLocation = Location;
      this->LastKnownLocation = -1;
      //this->LastKnownTime = -1;
    }

    item(std::string ID, std::string description, int AssignedLoc, int LastKnownLoc, bool inside){
      this->RFID = ID;
      this->name = description;
      this->AssignedLocation = AssignedLoc;
      this->LastKnownLocation = LastKnownLoc;
      inside_bag = inside;
      //this->LastKnownTime = -1;
    }


    
    item(const item &other){
      this->RFID = other.RFID;
      this->name = other.name;
      this->AssignedLocation = other.AssignedLocation;
      this->LastKnownLocation = other.LastKnownLocation;
      //this->LastKnownTime = other.LastKnownTime;
      this->inside_bag = other.inside_bag;
    }

    bool operator <(const item& rhs) const {
        return this->RFID < rhs.RFID;
    }

    void operator &=(const item& other) {
      RFID = other.RFID;
      name = other.name;
      AssignedLocation = other.AssignedLocation;
      LastKnownLocation = other.LastKnownLocation;
      //LastKnownTime = other.LastKnownTime;
      inside_bag = other.inside_bag;
    }
};

void printWell(std::string str, int offset = 0){
  for(int i = 0; i < str.length() - offset; ++i){
    Serial.print(str[i]);
  }
  Serial.println();
}

std::map<std::string, item> mymap;

void addNewItem(item a){
  mymap[a.RFID] = a;
}

void addNewItem(std::string ID, std::string description, int Location){
  item a (ID, description, Location);
  mymap[ID] = a;
}

void removeItem(std::string RFID){
  mymap.erase(RFID);
}

void registerItem(std::string new_name, int Location){
  for(auto& curr : mymap){
    if(curr.second.name == ""){
      curr.second.name = new_name;
      curr.second.AssignedLocation = Location;
      --unassigned_tags;
      //if(Location != -1)  ++activeTags;
      return;
    }
  }
}

void registerItembyQueue(std::string new_name, int Location){
  std::string ID = mydeque.front();
  mydeque.pop_front();
  mymap[ID].name = new_name;
  mymap[ID].AssignedLocation = Location;
  --unassigned_tags;
  //if(Location != -1)  ++activeTags;
}

void changeName(std::string old_name, std::string new_name){

  for(auto& curr : mymap){
    if(curr.second.name == old_name){
      curr.second.name = new_name;
      return;
    }
  }

}

void changeAssignedLocation(std::string name, int Location){

  for(auto& curr : mymap){
    if(curr.second.name == name){
      curr.second.AssignedLocation = Location;
      //if(Location == -1)  --activeTags;
      return;
    }
  }

}

// returns "present" 1 if the item was placed in the correct location in the bag
// returns "misplaced" -1 if the item was placed in the incorrect location
// returns "missing" 0 if the item was removed outside the bag

std::string findStatus(std::string ID){

  if(mymap[ID].AssignedLocation == -1 && mymap[ID].LastKnownLocation == -1)  return "outside";

  if(mymap[ID].inside_bag){
    if(mymap[ID].LastKnownLocation == mymap[ID].AssignedLocation) return "present";
    else                                                          return "misplaced";
  }

  return "missing";
}

std::string findStatusbyName(std::string name){
  for(const auto& curr : mymap){
    if(curr.second.name == name){
      return findStatus(curr.first);
    }
  }

  return "UNREGISTRED";
}

void relocate(std::string ID, int Location){ //, float time){

  mymap[ID].inside_bag = !mymap[ID].inside_bag;
  mymap[ID].LastKnownLocation = Location;
  //mymap[ID].LastKnownTime = time;

  if(mymap[ID].AssignedLocation == -1){
    //mymap[ID].AssignedLocation = Location;
    mydeque.push_back(ID);
    ++unassigned_tags;
  }
}

void scan(int location){
  // Fill the given tag and time arrays. 
  myRfids[location-1]->getAllTags(allTags);
  //myRfids[location-1]->getAllPrecTimes(allTimes);

  for( int i = 0; i < MAX_RFID_STORAGE; i++) {
    value = allTags[i].c_str();
    if(value == "000000") {
      return;
    }


    Serial.print("RFID Tag: "); 
    Serial.println(allTags[i]);
    //Serial.print(" Scan Time: ");
    //Serial.println(allTimes[i]);
    delay(100); //debug

    relocate(value, location); //, allTimes[i]);
  } 
}

// LEDS Stuff
void CheckLEDs(){

  for(int i = 0; i < 4; ++i){
    leds[i] = false;
  }

  for(const auto& curr : mymap){
    if( (!curr.second.inside_bag) || (curr.second.AssignedLocation != curr.second.LastKnownLocation) ){
      leds[curr.second.AssignedLocation - 1] = true;
    }
  }

  //if(activeTags == 0) leds[3] = false;
  //else 
  leds[3] = !leds[0] & !leds[1] & !leds[2];
}

//Bluetooth Stuff
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

class CharacteristicCallBack: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override { 
    std::string pChar2_value_stdstr = pChar->getValue();
    String pChar2_value_string = String(pChar2_value_stdstr.c_str());
    int pChar2_value_int = pChar2_value_string.toInt();
    Serial.println("pChar2: " + pChar2_value_string); 
  }
};

// My bluetooth functions

bool safeRead(std::string & name, bool equal, std::string old = "x"){

  auto condition = [equal](std::string a, std::string b) { 
    if(equal)   return a == b;
    if(a == "") return true;
    else        return a[0] != 'x';
  };

  do{
    delay(200);
    name = pCharacteristic_2->getValue();
  }
  while(condition(name, old));
  return true;

}



void Blueread(){
  if (deviceConnected) {
    Serial.println("Bluetooth Device Connected");
    std::string operation = pCharacteristic_2->getValue();
    std::string old_name, new_name, location;

    if(operation == "")     return;
    if(operation[0] == 'x') return;
    
    switch (operation[0]) {
      case '0': //register new item

        if(unassigned_tags > 0){
          pCharacteristic->setValue("0");
          pCharacteristic->notify();
          delay(10);
        }
        else{
          pCharacteristic->setValue("-1");
          pCharacteristic->notify();
          delay(500);

          Serial.println("Enter \"x\" ");
          pCharacteristic->setValue("a");
          pCharacteristic->notify();
          delay(10);

          if(safeRead(operation, false) == false)  break;
            return;
        }

        Serial.println("Enter new name:");

        if(safeRead(new_name, true, operation) == false){
          break;
        }

        pCharacteristic->setValue("00");
        pCharacteristic->notify();

        Serial.println("Enter new location:");
        if(safeRead(location, true, new_name) == false) break;

        Serial.println("Enter \"x\" ");
        pCharacteristic->setValue("a");
        pCharacteristic->notify();
        delay(10);

        if(safeRead(operation, false) == false)  break;

        registerItembyQueue(new_name, stoi(location));
        break;


      case '1': //update name

        pCharacteristic->setValue("1");
        pCharacteristic->notify();
        delay(10);

        Serial.println("Enter old name:");
        if(safeRead(old_name, true, operation) == false) break;

        pCharacteristic->setValue("11");
        pCharacteristic->notify();
        delay(10);

        Serial.println("Enter new name:");
        if(safeRead(new_name, true, old_name) == false) break;
        changeName(old_name, new_name);


        Serial.println("Enter \"x\" ");
        pCharacteristic->setValue("a");
        pCharacteristic->notify();
        delay(10);

        if(safeRead(operation, false) == false) break;

        break;


      case '2': //update location

        pCharacteristic->setValue("2");
        pCharacteristic->notify();
        delay(10);

        Serial.println("Enter name:");
        if(safeRead(old_name, true, operation) == false) break;

        pCharacteristic->setValue("22");
        pCharacteristic->notify();
        delay(10);

        Serial.println("Enter location:");
        if(safeRead(location, true, old_name) == false) break;
        changeAssignedLocation(old_name, stoi(location));

        Serial.println("Enter \"x\" ");
        pCharacteristic->setValue("a");
        pCharacteristic->notify();
        delay(10);

        if(safeRead(operation, false) == false) break;
        break;

      case '3': //send data
        while(operation[0] != 'x'){
          pCharacteristic->setValue("3");
          pCharacteristic->notify();
          delay(10);

          Serial.println("Enter name:");
          if(safeRead(old_name, true, operation) == false) break;

          pCharacteristic->setValue(findStatusbyName(old_name));
          pCharacteristic->notify();
          delay(10);

          Serial.println("Enter \"x\" to terminate");
          if(safeRead(operation, true, old_name) == false) break;
        }

        break;

      case '4': // resynch
        break;

      case '5': // reset

        Serial.println("Enter \"x\" ");
        pCharacteristic->setValue("a");
        pCharacteristic->notify();
        delay(10);

        if(safeRead(operation, false) == false) break;

        //  preferences.clear();
        unassigned_tags = 0;
        mymap.clear();
        Serial.println("Reset!!!");
        break;

      default:
        Serial.println("Invalid Operation");
    }

    pCharacteristic->setValue("x");
    pCharacteristic->notify();

    delay(100);
  
  }
  // disconnecting
  if (!deviceConnected && oldDeviceConnected) {
      delay(500); // give the bluetooth stack the chance to get things ready
      pServer->startAdvertising(); // restart advertising
      Serial.println("start advertising");
      oldDeviceConnected = deviceConnected;
  }
  // connecting
  if (deviceConnected && !oldDeviceConnected) {
      // do stuff here on connecting
      oldDeviceConnected = deviceConnected;
  }
}

// print statuses
void showStatus(){
  for(const auto& curr : mymap){
      std::string name = curr.second.name;
      std::string status = findStatus(curr.first);

      if(curr.second.AssignedLocation == -1)  continue;

      if(name == "")  name = "UNREGISTERED\0";

      Serial.print("Map Item and Status:\t");
      for(int i=0; i<name.length()-1; i++) {
        Serial.print(name[i]);
      }

      Serial.print("\t");

      for(int i = 0; i < status.length(); ++i){
        Serial.print(status[i]);
      }
      Serial.println();
  }
}

void save_values(){
  preferences.putUInt("size", mymap.size());
  int i = 0;

  for(const auto &curr : mymap){

    preferences.putString(("ID"     + std::to_string(i)).c_str(), (curr.first + "\0").c_str());                            // RFID
    preferences.putString(("name"   + std::to_string(i)).c_str(), (curr.second.name + "\0").c_str());                      // name
    preferences.putShort( ("AL"     + std::to_string(i)).c_str(), curr.second.AssignedLocation);          // Assigned location
    preferences.putShort( ("LKL"    + std::to_string(i)).c_str(), curr.second.LastKnownLocation);         // Last Known location
    preferences.putShort( ("inside" + std::to_string(i)).c_str(), curr.second.inside_bag);                // inside_bag
    ++i;
  }


  if(mymap.size() > 0){
    Serial.print("All the data (");
    Serial.print(mymap.size());
    Serial.print(" item");
    if(mymap.size() > 1) Serial.print("s");
    Serial.println(") was saved");
  }


  preferences.putUInt("queue", unassigned_tags);
  int j = 0;

  for(const auto & tag : mydeque){
    preferences.putString(("unregistered"     + std::to_string(j)).c_str(), (tag + "\0").c_str()); // unregistred IDs
    ++j;
  }

  if(unassigned_tags > 0){
    Serial.print("There are ");
    Serial.print(unassigned_tags);
    Serial.print(" unregistered tag");
    if(unassigned_tags > 1) Serial.print("s");
    Serial.println(" that were saved");
  }

}

void load_values(){
  unsigned int size = preferences.getUInt("size", 0);

  for(int i = 0; i < size; ++i){
    std::string ID      = preferences.getString(("ID"     + std::to_string(i)).c_str(), String("")).c_str();
    if(ID == "")  {
      Serial.println("Error at loading the saved data!");
      continue;
    }
    std::string name    = preferences.getString(("name"   + std::to_string(i)).c_str(), String("")).c_str();
    printWell(name);
    name += " ";
    printWell(name);
    int AssignedLoc     = preferences.getShort( ("AL"     + std::to_string(i)).c_str(), -1);
    int LastKnownLoc    = preferences.getShort( ("LKL"    + std::to_string(i)).c_str(), -1);
    bool inside         = preferences.getShort( ("inside" + std::to_string(i)).c_str(), false);


    item curr(ID, name, AssignedLoc, LastKnownLoc, inside);
    addNewItem(curr);
  }

  if(size > 0)   {
    Serial.print("All the data (");
    Serial.print(size);
    Serial.print(" item");
    if(size > 1) Serial.print("s");
    Serial.println(") was loaded");
  }


  unassigned_tags = preferences.getUInt("queue", 0);
  //mydeque.resize(unassigned_tags);

  for(int j = 0; j < unassigned_tags; ++j){
    mydeque.push_back(  preferences.getString( ("unregistered"     + std::to_string(j)).c_str(), String("000000") ).c_str() ); // unregistred IDs
  }


  if(unassigned_tags > 0){
    Serial.print("There are ");
    Serial.print(unassigned_tags);
    Serial.print(" unregistered tag");
    if(unassigned_tags > 1) Serial.print("s");
    Serial.println(" that were loaded");
  }
}

void setup() {

  // pins setup

  Wire.setPins(I2C_SDA, I2C_SCL);
  Wire.begin();

  Serial.begin(115200);

  pinMode(LED1_BUILTIN, OUTPUT);
  pinMode(LED2_BUILTIN, OUTPUT);
  pinMode(LED3_BUILTIN, OUTPUT);
  pinMode(LEDG_BUILTIN, OUTPUT);


  //timing setup
  for(int i = 0; i < NUMBER_OF_SENSORS; ++i){
    Timer0_Cfg[i] = NULL;
  }
  for(int i = 0; i < NUMBER_OF_SENSORS; ++i){
    Timer0_Cfg[i] = timerBegin(0, 80, true);;
  }

  // map setup for testing only

  //addNewItem("60141148227252", "test1", 1);
  //addNewItem("60141148227252", "test2", 2);
  //addNewItem("408323840145", "", -1); 
  //++unassigned_tags;

   

  // MUX setup (turned off due to fried MUX)
  /*
  if (myMuxRFID.begin(0x71, Wire) == false)
  {
    Serial.println("RFID Mux not detected. Freezing...");
    while (1);
  }
  Serial.println("RFID Mux detected");
  */

  /*
  if (myMuxLight.begin(0x70, Wire) == false)
  {
    Serial.println("Light sensor Mux not detected. Freezing...");
    while (1);
  }
  Serial.println("Light sensor Mux detected");
  */
  // RFID setup

  for(int i = 0; i < NUMBER_OF_SENSORS; ++i){

    myRfids[i] = new Qwiic_Rfid(RFID_ADDR);
    //  myMuxRFID.setPort(i);

    if(! myRfids[i]->begin())
      Serial.println("Could not communicate with Qwiic RFID!"); 
    else
      Serial.println("Ready to scan some tags!"); 

  }


  // Light Sensors
  
  for(int i = 0; i < NUMBER_OF_SENSORS; ++i){

    ltr[i] = Adafruit_LTR329();
    //myMuxLight.setPort(i);

    if ( ! ltr[i].begin() ) {
      Serial.println("Couldn't find LTR sensor!");
      while (1) delay(10);
    }
    Serial.println("Found LTR sensor!");

    ltr[i].setGain(LTR3XX_GAIN_2);
    Serial.print("Gain : ");
    switch (ltr[i].getGain()) {
      case LTR3XX_GAIN_1: Serial.println(1); break;
      case LTR3XX_GAIN_2: Serial.println(2); break;
      case LTR3XX_GAIN_4: Serial.println(4); break;
      case LTR3XX_GAIN_8: Serial.println(8); break;
      case LTR3XX_GAIN_48: Serial.println(48); break;
      case LTR3XX_GAIN_96: Serial.println(96); break;
    }

    ltr[i].setIntegrationTime(LTR3XX_INTEGTIME_100);
    Serial.print("Integration Time (ms): ");
    switch (ltr[i].getIntegrationTime()) {
      case LTR3XX_INTEGTIME_50: Serial.println(50); break;
      case LTR3XX_INTEGTIME_100: Serial.println(100); break;
      case LTR3XX_INTEGTIME_150: Serial.println(150); break;
      case LTR3XX_INTEGTIME_200: Serial.println(200); break;
      case LTR3XX_INTEGTIME_250: Serial.println(250); break;
      case LTR3XX_INTEGTIME_300: Serial.println(300); break;
      case LTR3XX_INTEGTIME_350: Serial.println(350); break;
      case LTR3XX_INTEGTIME_400: Serial.println(400); break;
    }

    ltr[i].setMeasurementRate(LTR3XX_MEASRATE_200);
    Serial.print("Measurement Rate (ms): ");
    switch (ltr[i].getMeasurementRate()) {
      case LTR3XX_MEASRATE_50: Serial.println(50); break;
      case LTR3XX_MEASRATE_100: Serial.println(100); break;
      case LTR3XX_MEASRATE_200: Serial.println(200); break;
      case LTR3XX_MEASRATE_500: Serial.println(500); break;
      case LTR3XX_MEASRATE_1000: Serial.println(1000); break;
      case LTR3XX_MEASRATE_2000: Serial.println(2000); break;
    }
  }


  // Create the BLE Device
  //BLE.setLocalName("TMAT");
  BLEDevice::init("TMAT");

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create the BLE Service
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create a BLE Characteristic
  pCharacteristic = pService->createCharacteristic(
                      CHAR1_UUID,
                      BLECharacteristic::PROPERTY_NOTIFY
                    );                   

  pCharacteristic_2 = pService->createCharacteristic(
                      CHAR2_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE  
                    );  

  // Create a BLE Descriptor
  
  pDescr = new BLEDescriptor((uint16_t)0x2901);
  pDescr->setValue("A very interesting variable");
  pCharacteristic->addDescriptor(pDescr);
  
  pBLE2902 = new BLE2902();
  pBLE2902->setNotifications(true);
  
  // Add all Descriptors here
  pCharacteristic->addDescriptor(pBLE2902);
  pCharacteristic_2->addDescriptor(new BLE2902());
  
  // After defining the desriptors, set the callback functions
  pCharacteristic_2->setCallbacks(new CharacteristicCallBack());
  
  // Start the service
  pService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);  // set value to 0x00 to not advertise this parameter
  BLEDevice::startAdvertising();
  Serial.println("Waiting a client connection to notify...");


  // saving

  //  preferences.begin("mapdata", false);
  //  preferences.clear();   //temp
//  
  //  load_values();


}

void loop() {

  for(int i = 0; i < 4; ++i){
    leds[i] = false;
  }

  bool valid;
  uint16_t visible_plus_ir[NUMBER_OF_SENSORS], infrared[NUMBER_OF_SENSORS];

  for(int i = 0; i < NUMBER_OF_SENSORS; ++i){


    //  myMuxLight.setPort(i);
    //  myMuxRFID.setPort(i);

    if (ltr[i].newDataAvailable()) {
      valid = ltr[i].readBothChannels(visible_plus_ir[i], infrared[NUMBER_OF_SENSORS]);
      if (valid) {
        Serial.print("CH0 Visible + IR: ");
        Serial.print(visible_plus_ir[i]);
        Serial.print("\t\tCH1 Infrared: ");
        Serial.println(infrared[NUMBER_OF_SENSORS]);
      }
    }

    //RFID stuff
    delay(300);
    scan(i+1);

  }


  //Indication Subsystem (LEDs) turn on
  CheckLEDs();

  for(int i = 0; i < NUMBER_OF_SENSORS; ++i){
    if(visible_plus_ir[i] > LightThreshold){

      //Do not turn on if time limit is reached
      if(timerReadSeconds(Timer0_Cfg[i]) > 30) {
        continue;
      }
      
      timerStart(Timer0_Cfg[i]);

      if(leds[i]) digitalWrite(LED_ADDR[i], HIGH);
      
      if(i == NUMBER_OF_SENSORS-1){
        if(leds[3]) digitalWrite(LED_ADDR[3], HIGH);
      }
    }
  }

  //Bluetooth Stuff

  Blueread();

  //save_values();
  showStatus();


  // Indication subsystem turn off
  delay(500);

  //LED Blink
  for(int i = 0; i < 4; ++i){
    digitalWrite(LED_ADDR[i], LOW);
  }

  // LED shut after closing the backpack
  for(int i = 0; i < NUMBER_OF_SENSORS; ++i){
    if(visible_plus_ir[i] <= LightThreshold){

      timerWrite(Timer0_Cfg[i], 0);
      timerStop(Timer0_Cfg[i]);

      digitalWrite(LED_ADDR[i], LOW);
      if(i == NUMBER_OF_SENSORS - 1)  digitalWrite(LED_ADDR[3], LOW);

    }
  }

  Serial.println();
}