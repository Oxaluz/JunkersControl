#include <Arduino.h>
#include <mqtt.h>
#include <configuration.h>
#include <main.h>
#include <telnet.h>
#include <heating.h>
#include <ArduinoJson.h>

//——————————————————————————————————————————————————————————————————————————————
//  MQTT Client (uses Wifi Client)
//——————————————————————————————————————————————————————————————————————————————
PubSubClient client(espClient);

CommandedValues commandedValues;

// \brief (Re)connect to MQTT broker
void reconnectMqtt()
{
  if (!WiFi.isConnected())
  {
    Serial.println("Can't connect to MQTT broker. [No Network]");
    return;
  }

  // Loop until we're reconnected
  while (!client.connected())
  {
    
    Serial.print("Attempting MQTT connection...");

    String clientId = generateClientId();
    // Attempt to connect
    if (client.connect(clientId.c_str(), configuration.Mqtt.User, configuration.Mqtt.Password))
    {
      Serial.println("connected");

      // Subscribe to parameters.
      client.subscribe(configuration.Mqtt.Topics.HeatingParameters);
      client.subscribe(configuration.Mqtt.Topics.WaterParameters);
      client.subscribe(configuration.Mqtt.Topics.StatusRequest);
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

// Returns a client id for MQTT communication
String generateClientId()
{
  String macAddress = WiFi.macAddress();
  macAddress.replace(":", "");
  // Create client ID using MAC address
  String clientId = "ESP-";
  clientId += macAddress;

  return clientId;
}

void setupMqttClient()
{
  // Setup MQTT client
  client.setServer(configuration.Mqtt.Server, configuration.Mqtt.Port);
  client.setCallback(callback);
  client.setKeepAlive(10);
}

// Callback for MQTT subscribed topics
void callback(char *topic, byte *payload, unsigned int length)
{
  payload[length] = '\0';
  String s = String((char *)payload);
  if (!s)
  {
    return;
  }

  // Status Requested
  if (strcmp(topic, configuration.Mqtt.Topics.StatusRequest) == 0)
  {
    StaticJsonDocument<256> doc;

    DeserializationError error = deserializeJson(doc, (char *)payload, length);

    if (error)
    {
      WriteToConsoles("[Status Request] Error Processing JSON: ");
      WriteToConsoles(error.c_str());
      WriteToConsoles("\r\n");
      return;
    }
    /* Example JSON:
        {
            "HeatingTemperatures": true,
            "WaterTemperatures": true,
            "AuxilaryTemperatures": true,
            "Status": true
        }
    */
    bool HeatingTemperatures = doc["HeatingTemperatures"];   // false
    bool WaterTemperatures = doc["WaterTemperatures"];       // false
    bool AuxilaryTemperatures = doc["AuxilaryTemperatures"]; // true
    bool Status = doc["Status"];                             // false

    if (HeatingTemperatures) PublishHeatingTemperatures();    

    if (WaterTemperatures) PublishWaterTemperatures();

    if (AuxilaryTemperatures) PublishAuxilaryTemperatures();

    if (Status) PublishStatus();

  }

  // Receiving Heating Parameters
  if (strcmp(topic, configuration.Mqtt.Topics.HeatingParameters) == 0)
  {
    const int docSize = 384;
    StaticJsonDocument<docSize> doc;
    bool setFeedImmediately = false;
    DeserializationError error = deserializeJson(doc, (char *)payload, length);

    if (error)
    {
      WriteToConsoles("[Heating Parameters] Error Processing JSON: ");
      WriteToConsoles(error.c_str());
      WriteToConsoles("\r\n");
      return;
    }

    //Request Enable/Disable Heating and set the status of the heating accordingly
    commandedValues.Heating.Active = doc["Enabled"];

    commandedValues.Heating.FeedSetpoint = doc["FeedSetpoint"];
    commandedValues.Heating.BasepointTemperature = doc["FeedBaseSetpoint"];
    commandedValues.Heating.EndpointTemperature = doc["FeedCutOff"];    
    commandedValues.Heating.MinimumFeedTemperature = doc["FeedMinimum"];    
    commandedValues.Heating.AuxilaryTemperature = doc["AuxilaryTemperature"];
    commandedValues.Heating.AmbientTemperature = doc["AmbientTemperature"];
    commandedValues.Heating.TargetAmbientTemperature = doc["TargetAmbientTemperature"];

    
    if (doc["OnDemandBoost"] != commandedValues.Heating.Boost)
    {
      commandedValues.Heating.Boost = doc["OnDemandBoost"];
      commandedValues.Heating.BoostTimeCountdown = commandedValues.Heating.BoostDuration;
      setFeedImmediately = true;
    }

    commandedValues.Heating.BoostDuration = doc["OnDemandBoostDuration"];
    if (doc["FastHeatup"] != commandedValues.Heating.FastHeatup)
    {
      setFeedImmediately = true;
      // Set reference Temperature
      commandedValues.Heating.ReferenceAmbientTemperature = commandedValues.Heating.AmbientTemperature;
    }

    commandedValues.Heating.FastHeatup = doc["FastHeatup"];    
    commandedValues.Heating.FeedAdaption = doc["Adaption"];    
    commandedValues.Heating.ValveScaling = doc["ValveScaling"];    
    commandedValues.Heating.MaxValveOpening = doc["ValveScalingMaxOpening"];    
    commandedValues.Heating.ValveOpening = doc["ValveScalingOpening"];    
    commandedValues.Heating.DynamicAdaption = doc["DynamicAdaption"];    
    commandedValues.Heating.OverrideSetpoint = doc["OverrideSetpoint"];

    if (setFeedImmediately)
      SetFeedTemperature();

    // Receiving Water Parameters
    if (strcmp(topic, configuration.Mqtt.Topics.WaterParameters) == 0)
    {
      const int docSize = 16;
      StaticJsonDocument<docSize> doc;
      DeserializationError error = deserializeJson(doc, (char *)payload, length);

      if (error)
      {
        WriteToConsoles("[Water Parameters] Error Processing JSON: ");
        WriteToConsoles(error.c_str());
        WriteToConsoles("\r\n");
        return;
      }

      // TODO: Water Temperature Setpoint variable to be populated here...
      commandedValues.HotWater.SetPoint = doc["Setpoint"]; // 22.1

    }
  }
}

void PublishStatus()
{
  /* Example JSON
  {
			"GasBurner": true,
			"Pump": true,
			"Error": 0..255,
			"Season": true,
			"Working": true,
			"Boost": true,
			"FastHeatup": true			
  }
  */
  StaticJsonDocument<384> doc;
  JsonObject jsonObj = doc.to<JsonObject>();
  jsonObj["GasBurner"] = ceraValues.General.FlameLit;
  jsonObj["Pump"] = ceraValues.Heating.PumpActive;
  jsonObj["Error"] = ceraValues.General.Error;
  jsonObj["Season"] = ceraValues.Heating.Season;
  jsonObj["Working"] = ceraValues.Heating.Active;
  jsonObj["Boost"] = commandedValues.Heating.Boost;
  jsonObj["FastHeatup"] = commandedValues.Heating.FastHeatup;

  // Publish Data on MQTT
  char buffer[768];
  size_t n = serializeJson(doc, buffer);
  client.publish(configuration.Mqtt.Topics.Status, buffer, n);
}

void PublishHeatingTemperatures()
{
  /* Example JSON
  {
			"FeedMaximum": 75.10,
			"FeedCurrent": 30.10,
			"FeedSetpoint": 10.10,
			"Outside": 15.10
  }
  */

  StaticJsonDocument<384> doc;
  JsonObject jsonObj = doc.to<JsonObject>();
  jsonObj["FeedMaximum"] = ceraValues.Heating.FeedMaximum;
  jsonObj["FeedCurrent"] = ceraValues.Heating.FeedCurrent;
  jsonObj["FeedSetpoint"] = commandedValues.Heating.CalculatedFeedSetpoint;
  jsonObj["Outside"] = ceraValues.General.OutsideTemperature;

  // Publish Data on MQTT
  char buffer[768];
  size_t n = serializeJson(doc, buffer);
  client.publish(configuration.Mqtt.Topics.HeatingValues, buffer, n);
}

void PublishWaterTemperatures()
{
  //TODO: Gather HW temperatures
  /* Example JSON
		{
			"Maximum": 75.10,
			"Current": 30.10,
			"Setpoint": 10.10,
      ""
		}
  */

  StaticJsonDocument<384> doc;
  JsonObject jsonObj = doc.to<JsonObject>();
  jsonObj["Maximum"] = ceraValues.Hotwater.MaximumTemperature;
  jsonObj["Current"] = ceraValues.Hotwater.TemperatureCurrent;
  jsonObj["Setpoint"] = ceraValues.Hotwater.SetPoint;
  jsonObj["CFSetpoint"] = ceraValues.Hotwater.ContinousFlowSetpoint;
  jsonObj["Now"] = ceraValues.Hotwater.Now;
  jsonObj["Buffer"] = ceraValues.Hotwater.BufferMode;

  // Publish Data on MQTT
  char buffer[768];
  size_t n = serializeJson(doc, buffer);
  client.publish(configuration.Mqtt.Topics.WaterValues, buffer, n);
}

void PublishAuxilaryTemperatures()
{
  /*
  {

			"Feed": 30.10,
			"Return": 30.10,
			"Exhaust": 50.10,
			"Ambient": 17.10

  }
  */

  StaticJsonDocument<384> doc;
  JsonObject jsonObj = doc.to<JsonObject>();
  for (size_t i = 0; i < configuration.TemperatureSensors.SensorCount; i++)
  {
    Sensor curSensor = configuration.TemperatureSensors.Sensors[i];
    jsonObj[curSensor.Label] = ceraValues.Auxilary.Temperatures[i];
  }

  // Publish Data on MQTT
  char buffer[768];
  size_t n = serializeJson(doc, buffer);
  client.publish(configuration.Mqtt.Topics.AuxilaryValues, buffer, n);
}