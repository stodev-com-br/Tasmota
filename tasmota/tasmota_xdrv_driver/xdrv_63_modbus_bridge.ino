/*
  xdrv_63_modbus_bridge.ino - modbus bridge support for Tasmota

  Copyright (C) 2021  Theo Arends and Jeroenst

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#if defined(USE_MODBUS_BRIDGE)
/*********************************************************************************************\
 * Modbus Bridge using Modbus library (TasmotaModbus)
 *
 * Can be used trough web/mqtt commands and also via direct TCP connection (when defined)
 *
 * When USE_MODBUS_BRIDGE_TCP is also defined, this bridge can also be used as an ModbusTCP
 * bridge.
 *
 * Example Command:
 *   -- Read Input Register --
 *   ModbusSend {"deviceaddress": 1, "functioncode": 3, "startaddress": 1, "type":"uint16", "count":2}
 * 
 *   -- Write multiple coils --
 *   ModbusSend {"deviceaddress": 1, "functioncode": 15, "startaddress": 1, "type":"uint16", "count":4, "values":[1,2,3,4]}
\*********************************************************************************************/

#define XDRV_63 63

#define MBR_MAX_VALUE_LENGTH 30
#define MBR_BAUDRATE TM_MODBUS_BAUDRATE
#define MBR_MAX_REGISTERS 64

#define D_CMND_MODBUS_SEND "Send"
#define D_CMND_MODBUS_SETBAUDRATE "Baudrate"
#define D_CMND_MODBUS_SETSERIALCONFIG "SerialConfig"

#define D_JSON_MODBUS_RECEIVED "ModbusReceived"
#define D_JSON_MODBUS_DEVICE_ADDRESS "DeviceAddress"
#define D_JSON_MODBUS_FUNCTION_CODE "FunctionCode"
#define D_JSON_MODBUS_START_ADDRESS "StartAddress"
#define D_JSON_MODBUS_COUNT "Count"
#define D_JSON_MODBUS_ENDIAN "Endian"
#define D_JSON_MODBUS_TYPE "Type" // allready defined
#define D_JSON_MODBUS_VALUES "Values"
#define D_JSON_MODBUS_LENGTH "Length"

#ifndef USE_MODBUS_BRIDGE_TCP
const char kModbusBridgeCommands[] PROGMEM = "Modbus|" // Prefix
    D_CMND_MODBUS_SEND "|" D_CMND_MODBUS_SETBAUDRATE "|" D_CMND_MODBUS_SETSERIALCONFIG;

void (*const ModbusBridgeCommand[])(void) PROGMEM = {
    &CmndModbusBridgeSend, &CmndModbusBridgeSetBaudrate, &CmndModbusBridgeSetConfig};
#endif

#ifdef USE_MODBUS_BRIDGE_TCP

#define MODBUS_BRIDGE_TCP_CONNECTIONS 1 // number of maximum parallel connections, only 1 supported with modbus
#define MODBUS_BRIDGE_TCP_BUF_SIZE 255  // size of the buffer, above 132 required for efficient XMODEM

#define D_CMND_MODBUS_TCP_START "TCPStart"
#define D_CMND_MODBUS_TCP_CONNECT "TCPConnect"

const char kModbusBridgeCommands[] PROGMEM = "Modbus|" // Prefix
    D_CMND_MODBUS_TCP_START "|" D_CMND_MODBUS_TCP_CONNECT "|" D_CMND_MODBUS_SEND "|" D_CMND_MODBUS_SETBAUDRATE "|" D_CMND_MODBUS_SETSERIALCONFIG;

void (*const ModbusBridgeCommand[])(void) PROGMEM = {
    &CmndModbusTCPStart, &CmndModbusTCPConnect,
    &CmndModbusBridgeSend, &CmndModbusBridgeSetBaudrate, &CmndModbusBridgeSetConfig};

struct ModbusBridgeTCP
{
  WiFiServer *server_tcp = nullptr;
  WiFiClient client_tcp[MODBUS_BRIDGE_TCP_CONNECTIONS];
  uint8_t client_next = 0;
  uint8_t *tcp_buf = nullptr; // data transfer buffer
  IPAddress ip_filter;
  uint16_t tcp_transaction_id = 0;
};

ModbusBridgeTCP modbusBridgeTCP;
#endif

#include <TasmotaModbus.h>
TasmotaModbus *tasmotaModbus = nullptr;

enum class ModbusBridgeError
{
  noerror = 0,
  nodataexpected = 1,
  wrongdeviceaddress = 2,
  wrongfunctioncode = 3,
  wrongstartaddress = 4,
  wrongtype = 5,
  wrongdataCount = 6,
  wrongcount = 7,
  tomanydata = 8
};

enum class ModbusBridgeFunctionCode
{
  mb_undefined = 0,
  mb_readCoilStatus = 1,
  mb_readContactStatus = 2,
  mb_readHoldingRegisters = 3,
  mb_readInputRegisters = 4,
  mb_writeSingleCoil = 5,
  mb_writeSingleRegister = 6,
  mb_writeMultipleCoils = 15,
  mb_writeMultipleRegisters = 16
};

enum class ModbusBridgeType
{
  mb_undefined,
  mb_uint8,
  mb_uint16,
  mb_uint32,
  mb_int8,
  mb_int16,
  mb_int32,
  mb_float,
  mb_raw,
  mb_hex,
  mb_bit
};

enum class ModbusBridgeEndian
{
  mb_undefined,
  mb_msb,
  mb_lsb
};

struct ModbusBridge
{
  unsigned long polling_window = 0;
  int in_byte_counter = 0;

  ModbusBridgeFunctionCode functionCode = ModbusBridgeFunctionCode::mb_undefined;
  ModbusBridgeType type = ModbusBridgeType::mb_undefined;

  uint16_t dataCount = 0;
  uint16_t startAddress = 0;
  uint8_t deviceAddress = 0;
  uint8_t count = 0;
  bool raw = false;
};

ModbusBridge modbusBridge;

/********************************************************************************************/
//
// Applies serial configuration to modbus serial port
//
bool ModbusBridgeBegin(void)
{
  if ((Settings->modbus_sbaudrate < 1) || (Settings->modbus_sbaudrate > (115200 / 300)))
    Settings->modbus_sbaudrate = (uint8_t)((uint32_t)MBR_BAUDRATE / 300);
  if (Settings->modbus_sconfig > TS_SERIAL_8O2)
    Settings->modbus_sconfig = TS_SERIAL_8N1;

  int result = tasmotaModbus->Begin(Settings->modbus_sbaudrate * 300, ConvertSerialConfig(Settings->modbus_sconfig)); // Reinitialize modbus port with new baud rate
  if (result)
  {
    if (2 == result)
    {
      ClaimSerial();
    }
    AddLog(LOG_LEVEL_DEBUG, PSTR("MBS: MBR %s ser init at %d baud"), (2 == result ? "HW" : "SW"), Settings->modbus_sbaudrate * 300);
  }
  return result;
}

void SetModbusBridgeConfig(uint32_t serial_config)
{
  if (serial_config > TS_SERIAL_8O2)
  {
    serial_config = TS_SERIAL_8N1;
  }
  if (serial_config != Settings->modbus_sconfig)
  {
    Settings->modbus_sconfig = serial_config;
    ModbusBridgeBegin();
  }
}

void SetModbusBridgeBaudrate(uint32_t baudrate)
{
  if ((baudrate >= 300) && (baudrate <= 115200))
  {
    if (baudrate / 300 != Settings->modbus_sbaudrate)
    {
      Settings->modbus_sbaudrate = baudrate / 300;
      ModbusBridgeBegin();
    }
  }
}

/********************************************************************************************/
//
// Handles data received from tasmota modbus wrapper and send this to (TCP or) MQTT client
//
void ModbusBridgeHandle(void)
{
  bool data_ready = tasmotaModbus->ReceiveReady();
  if (data_ready)
  {
    uint8_t *buffer;
    buffer = (uint8_t *)malloc(9 + (modbusBridge.dataCount * 2)); // Addres(1), Function(1), Length(1), Data(1..n), CRC(2)
    uint32_t error = tasmotaModbus->ReceiveBuffer(buffer, modbusBridge.dataCount);

    if (error)
    {
      AddLog(LOG_LEVEL_DEBUG, PSTR("MBS: MBR Driver receive error %d"), error);
      free(buffer);
      return;
    }

#ifdef USE_MODBUS_BRIDGE_TCP
    for (uint32_t i = 0; i < nitems(modbusBridgeTCP.client_tcp); i++)
    {
      WiFiClient &client = modbusBridgeTCP.client_tcp[i];
      if (client)
      {
        uint8_t MBAP_Header[7];
        MBAP_Header[0] = modbusBridgeTCP.tcp_transaction_id >> 8;
        MBAP_Header[1] = modbusBridgeTCP.tcp_transaction_id;
        MBAP_Header[2] = 0;
        MBAP_Header[3] = 0;
        MBAP_Header[4] = ((modbusBridge.dataCount * 2) + 3) >> 8;
        MBAP_Header[5] = (modbusBridge.dataCount * 2) + 3;
        MBAP_Header[6] = buffer[0]; // Send slave address
        client.write(MBAP_Header, 7);
        client.write(buffer + 1, 1); // Send Functioncode
        uint8_t bytecount[1];
        bytecount[0] = modbusBridge.dataCount * 2;
        client.write(bytecount, 1);                             // Send length of rtu data
        client.write(buffer + 3, (modbusBridge.dataCount * 2)); // Don't send CRC
        client.flush();
        AddLog(LOG_LEVEL_DEBUG, PSTR("MBS: MBRTCP from Modbus deviceAddress %d, writing %d bytes to client"), buffer[0], (modbusBridge.dataCount * 2) + 9);
      }
    }
#endif

    ModbusBridgeError errorcode = ModbusBridgeError::noerror;
    if (modbusBridge.deviceAddress == 0)
    {
#ifdef USE_MODBUS_BRIDGE_TCP
      // If tcp client connected don't log error and exit this function (do not process)
      if (nitems(modbusBridgeTCP.client_tcp))
      {
        free(buffer);
        return;
      }
#endif
      errorcode = ModbusBridgeError::nodataexpected;
    }
    else if (modbusBridge.deviceAddress != (uint8_t)buffer[0])
      errorcode = ModbusBridgeError::wrongdeviceaddress;
    else if ((uint8_t)modbusBridge.functionCode != (uint8_t)buffer[1])
      errorcode = ModbusBridgeError::wrongfunctioncode;
    else if ((uint8_t)modbusBridge.functionCode < 5)
    {
      if ((uint8_t)modbusBridge.functionCode < 3)
      {
        if ((uint8_t)(((modbusBridge.dataCount - 1) >> 3) + 1) != (uint8_t)buffer[2])
          errorcode = ModbusBridgeError::wrongdataCount;
      }
      else
      {
        if ((modbusBridge.type == ModbusBridgeType::mb_int8 || modbusBridge.type == ModbusBridgeType::mb_uint8)
          && ((uint8_t)modbusBridge.dataCount * 2 != (uint8_t)buffer[2]))
          errorcode = ModbusBridgeError::wrongdataCount;
        else if ((modbusBridge.type == ModbusBridgeType::mb_bit)
          && ((uint8_t)modbusBridge.dataCount * 2 != (uint8_t)buffer[2]))
          errorcode = ModbusBridgeError::wrongdataCount;
        else if ((modbusBridge.type == ModbusBridgeType::mb_int16 || modbusBridge.type == ModbusBridgeType::mb_uint16)
          && ((uint8_t)modbusBridge.dataCount * 2 != (uint8_t)buffer[2]))
          errorcode = ModbusBridgeError::wrongdataCount;
        else if ((modbusBridge.type == ModbusBridgeType::mb_int32 || modbusBridge.type == ModbusBridgeType::mb_uint32 || modbusBridge.type == ModbusBridgeType::mb_float)
          && ((uint8_t)modbusBridge.dataCount * 2 != (uint8_t)buffer[2]))
          errorcode = ModbusBridgeError::wrongdataCount;
      }
    }
    if (errorcode == ModbusBridgeError::noerror)
    {
      if (modbusBridge.type == ModbusBridgeType::mb_raw)
      {
        Response_P(PSTR("{\"" D_JSON_MODBUS_RECEIVED "\":{\"RAW\":["));
        for (uint8_t i = 0; i < tasmotaModbus->ReceiveCount(); i++)
        {
          ResponseAppend_P(PSTR("%d"), buffer[i]);
          if (i < tasmotaModbus->ReceiveCount() - 1)
            ResponseAppend_P(PSTR(","));
        }
        ResponseAppend_P(PSTR("]}"));
        ResponseJsonEnd();
        MqttPublishPrefixTopicRulesProcess_P(RESULT_OR_TELE, PSTR(D_JSON_MODBUS_RECEIVED));
      }
      else if (modbusBridge.type == ModbusBridgeType::mb_hex)
      {
        Response_P(PSTR("{\"" D_JSON_MODBUS_RECEIVED "\":{\"HEX\":["));
        for (uint8_t i = 0; i < tasmotaModbus->ReceiveCount(); i++)
        {
          ResponseAppend_P(PSTR("0x%02X"), buffer[i]);
          if (i < tasmotaModbus->ReceiveCount() - 1)
            ResponseAppend_P(PSTR(","));
        }
        ResponseAppend_P(PSTR("]}"));
        ResponseJsonEnd();
        MqttPublishPrefixTopicRulesProcess_P(RESULT_OR_TELE, PSTR(D_JSON_MODBUS_RECEIVED));
      }
      else if ((buffer[1] > 0) && (buffer[1] < 7)) // Read Registers
      {
        uint8_t dataOffset = 3;
        Response_P(PSTR("{\"" D_JSON_MODBUS_RECEIVED "\":{"));
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_DEVICE_ADDRESS "\":%d,"), buffer[0]);
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_FUNCTION_CODE "\":%d,"), buffer[1]);
        if (buffer[1] < 5)
        {
          ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_START_ADDRESS "\":%d,"), modbusBridge.startAddress);
        }
        else
        {
          ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_START_ADDRESS "\":%d,"), (buffer[2] << 8) + buffer[3]);
          dataOffset = 4;
        }
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_LENGTH "\":%d,"), tasmotaModbus->ReceiveCount());
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_COUNT "\":%d,"), modbusBridge.count);
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_VALUES "\":["));

        uint8_t data_count = modbusBridge.count;
        if ((uint8_t)modbusBridge.functionCode < 3)
        {
          if (modbusBridge.type == ModbusBridgeType::mb_int8 || modbusBridge.type == ModbusBridgeType::mb_uint8)
            data_count = (uint8_t)(((modbusBridge.count - 1) >> 3) + 1);
          else if (modbusBridge.type == ModbusBridgeType::mb_int16 || modbusBridge.type == ModbusBridgeType::mb_uint16)
            data_count = (uint8_t)(((modbusBridge.count - 1) >> 4) + 1);
          else if (modbusBridge.type == ModbusBridgeType::mb_int32 || modbusBridge.type == ModbusBridgeType::mb_uint32 || modbusBridge.type == ModbusBridgeType::mb_float)
            data_count = (uint8_t)(((modbusBridge.count - 1) >> 5) + 1);
        }
        for (uint8_t count = 0; count < data_count; count++)
        {
          char svalue[MBR_MAX_VALUE_LENGTH + 1] = "";
          if (modbusBridge.type == ModbusBridgeType::mb_float)
          {
            float value = 0;
            if (buffer[1] < 3)
            {
              if (buffer[2] - (count * 4))
                ((uint8_t *)&value)[0] = buffer[dataOffset + (count * 4)]; // Get int values
              if ((buffer[2] - (count * 4)) >> 1)
                ((uint8_t *)&value)[1] = buffer[dataOffset + 1 + (count * 4)];
              if ((buffer[2] - (count * 4) - 1) >> 1)
                ((uint8_t *)&value)[2] = buffer[dataOffset + 2 + (count * 4)];
              if ((buffer[2] - (count * 4)) >> 2)
                ((uint8_t *)&value)[3] = buffer[dataOffset + 3 + (count * 4)];
            }
            else
            {
              ((uint8_t *)&value)[3] = buffer[dataOffset + (count * 4)]; // Get float values
              ((uint8_t *)&value)[2] = buffer[dataOffset + 1 + (count * 4)];
              ((uint8_t *)&value)[1] = buffer[dataOffset + 2 + (count * 4)];
              ((uint8_t *)&value)[0] = buffer[dataOffset + 3 + (count * 4)];
            }
            ext_snprintf_P(svalue, sizeof(svalue), "%*_f", 10, &value);
          }
          else if (modbusBridge.type == ModbusBridgeType::mb_bit)
          {
            uint8_t value = (uint8_t)(buffer[dataOffset + (count >> 3)]);
            snprintf(svalue, MBR_MAX_VALUE_LENGTH, "%d", ((value >> (count & 7)) & 1));
          }
          else
          {
            if ((modbusBridge.type == ModbusBridgeType::mb_int32) ||
                (modbusBridge.type == ModbusBridgeType::mb_uint32))
            {
              uint32_t value = 0;
              if (buffer[1] < 3)
              {
                if (buffer[2] - (count * 4))
                  ((uint8_t *)&value)[0] = buffer[dataOffset + (count * 4)]; // Get int values
                if ((buffer[2] - (count * 4)) >> 1)
                  ((uint8_t *)&value)[1] = buffer[dataOffset + 1 + (count * 4)];
                if ((buffer[2] - (count * 4) - 1) >> 1)
                  ((uint8_t *)&value)[2] = buffer[dataOffset + 2 + (count * 4)];
                if ((buffer[2] - (count * 4)) >> 2)
                  ((uint8_t *)&value)[3] = buffer[dataOffset + 3 + (count * 4)];
              }
              else
              {
                ((uint8_t *)&value)[3] = buffer[dataOffset + (count * 4)]; // Get int values
                ((uint8_t *)&value)[2] = buffer[dataOffset + 1 + (count * 4)];
                ((uint8_t *)&value)[1] = buffer[dataOffset + 2 + (count * 4)];
                ((uint8_t *)&value)[0] = buffer[dataOffset + 3 + (count * 4)];
              }
              if (modbusBridge.type == ModbusBridgeType::mb_int32)
                snprintf(svalue, MBR_MAX_VALUE_LENGTH, "%d", value);
              else
                snprintf(svalue, MBR_MAX_VALUE_LENGTH, "%u", value);
            }
            else if ((modbusBridge.type == ModbusBridgeType::mb_int16) ||
                     (modbusBridge.type == ModbusBridgeType::mb_uint16))
            {
              uint16_t value = 0;
              if (buffer[1] < 3)
              {
                if (buffer[2] - (count * 2))
                  ((uint8_t *)&value)[0] = buffer[dataOffset + (count * 2)];
                if ((buffer[2] - (count * 2)) >> 1)
                  ((uint8_t *)&value)[1] = buffer[dataOffset + 1 + (count * 2)];
              }
              else
              {
                ((uint8_t *)&value)[1] = buffer[dataOffset + (count * 2)];
                ((uint8_t *)&value)[0] = buffer[dataOffset + 1 + (count * 2)];
              }
              if (modbusBridge.type == ModbusBridgeType::mb_int16)
                snprintf(svalue, MBR_MAX_VALUE_LENGTH, "%d", value);
              else
                snprintf(svalue, MBR_MAX_VALUE_LENGTH, "%u", value);
            }
            else if ((modbusBridge.type == ModbusBridgeType::mb_int8) ||
                     (modbusBridge.type == ModbusBridgeType::mb_uint8))
            {
              uint8_t value = buffer[dataOffset + (count * 1)];
              if (modbusBridge.type == ModbusBridgeType::mb_int8)
                snprintf(svalue, MBR_MAX_VALUE_LENGTH, "%d", value);
              else
                snprintf(svalue, MBR_MAX_VALUE_LENGTH, "%u", value);
            }
            /*
            else if (modbusBridge.type == ModbusBridgeType::mb_bit)
            {
              uint8_t value = (uint8_t)(buffer[dataOffset + count]);
              snprintf(svalue, MBR_MAX_VALUE_LENGTH, "%d%d%d%d%d%d%d%d", ((value >> 7) & 1), ((value >> 6) & 1), ((value >> 5) & 1), ((value >> 4) & 1), ((value >> 3) & 1), ((value >> 2) & 1), ((value >> 1) & 1), (value & 1));
            }*/
          }
          ResponseAppend_P(PSTR("%s"), svalue);
          if (count < data_count - 1)
            ResponseAppend_P(PSTR(","));
        }

        ResponseAppend_P(PSTR("]}"));
        ResponseJsonEnd();

        if (errorcode == ModbusBridgeError::noerror)
          MqttPublishPrefixTopicRulesProcess_P(RESULT_OR_TELE, PSTR(D_JSON_MODBUS_RECEIVED));
      }
      else if ((buffer[1] == 15) || (buffer[1] == 16)) // Write Multiple Registers
      {
        Response_P(PSTR("{\"" D_JSON_MODBUS_RECEIVED "\":{"));
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_DEVICE_ADDRESS "\":%d,"), buffer[0]);
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_FUNCTION_CODE "\":%d,"), buffer[1]);
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_START_ADDRESS "\":%d,"), (buffer[2] << 8) + buffer[3]);
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_LENGTH "\":%d,"), tasmotaModbus->ReceiveCount());
        ResponseAppend_P(PSTR("\"" D_JSON_MODBUS_COUNT "\":%d"), (buffer[4] << 8) + buffer[5]);
        ResponseAppend_P(PSTR("}"));
        ResponseJsonEnd();
        if (errorcode == ModbusBridgeError::noerror)
          MqttPublishPrefixTopicRulesProcess_P(RESULT_OR_TELE, PSTR(D_JSON_MODBUS_RECEIVED));
      }
      else
        errorcode = ModbusBridgeError::wrongfunctioncode;
    }
    if (errorcode != ModbusBridgeError::noerror)
    {
      AddLog(LOG_LEVEL_DEBUG, PSTR("MBS: MBR Recv Error %d"), (uint8_t)errorcode);
    }
    modbusBridge.deviceAddress = 0;
    free(buffer);
  }
}

/********************************************************************************************/
//
// Inits the tasmota modbus driver, sets serialport and if TCP enabled allocates a TCP buffer
//
void ModbusBridgeInit(void)
{
  if (PinUsed(GPIO_MBR_RX) && PinUsed(GPIO_MBR_TX))
  {
    tasmotaModbus = new TasmotaModbus(Pin(GPIO_MBR_RX), Pin(GPIO_MBR_TX));
    ModbusBridgeBegin();
#ifdef USE_MODBUS_BRIDGE_TCP
    // If TCP bridge is enabled allocate a TCP receive buffer
    modbusBridgeTCP.tcp_buf = (uint8_t *)malloc(MODBUS_BRIDGE_TCP_BUF_SIZE);
    if (!modbusBridgeTCP.tcp_buf)
    {
      AddLog(LOG_LEVEL_ERROR, PSTR("MBS: MBRTCP could not allocate buffer"));
      return;
    }
#endif
  }
}

#ifdef USE_MODBUS_BRIDGE_TCP
/********************************************************************************************/
//
// Handles data for TCP server and TCP client. Sends requests to Modbus Devices
//
void ModbusTCPHandle(void)
{
  uint8_t c;
  bool busy; // did we transfer some data?
  int32_t buf_len;

  if (!tasmotaModbus)
    return;

  // check for a new client connection
  if ((modbusBridgeTCP.server_tcp) && (modbusBridgeTCP.server_tcp->hasClient()))
  {
    WiFiClient new_client = modbusBridgeTCP.server_tcp->available();

    AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_TCP "MBS: MBRTCP Got connection from %s"), new_client.remoteIP().toString().c_str());
    // Check for IP filtering if it's enabled.
    if (modbusBridgeTCP.ip_filter)
    {
      if (modbusBridgeTCP.ip_filter != new_client.remoteIP())
      {
        AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_TCP "MBS: MBRTCP Rejected due to filtering"));
        new_client.stop();
      }
      else
      {
        AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_TCP "MBS: MBRTCP Allowed through filter"));
      }
    }

    // find an empty slot
    uint32_t i;
    for (i = 0; i < nitems(modbusBridgeTCP.client_tcp); i++)
    {
      WiFiClient &client = modbusBridgeTCP.client_tcp[i];
      if (!client)
      {
        client = new_client;
        break;
      }
    }
    if (i >= nitems(modbusBridgeTCP.client_tcp))
    {
      i = modbusBridgeTCP.client_next++ % nitems(modbusBridgeTCP.client_tcp);
      WiFiClient &client = modbusBridgeTCP.client_tcp[i];
      client.stop();
      client = new_client;
    }
  }

  do
  {
    busy = false; // exit loop if no data was transferred

    // handle data received from TCP
    for (uint32_t i = 0; i < nitems(modbusBridgeTCP.client_tcp); i++)
    {
      WiFiClient &client = modbusBridgeTCP.client_tcp[i];
      buf_len = 0;
      while (client && (buf_len < MODBUS_BRIDGE_TCP_BUF_SIZE) && (client.available()))
      {
        c = client.read();
        if (c >= 0)
        {
          modbusBridgeTCP.tcp_buf[buf_len++] = c;
          busy = true;
        }
      }
      if (buf_len == 12)
      {
        uint8_t mbdeviceaddress = (uint8_t)modbusBridgeTCP.tcp_buf[6];
        uint8_t mbfunctioncode = (uint8_t)modbusBridgeTCP.tcp_buf[7];
        uint16_t mbstartaddress = (uint16_t)((((uint16_t)modbusBridgeTCP.tcp_buf[8]) << 8) | ((uint16_t)modbusBridgeTCP.tcp_buf[9]));
        modbusBridge.dataCount = (uint16_t)((((uint16_t)modbusBridgeTCP.tcp_buf[10]) << 8) | ((uint16_t)modbusBridgeTCP.tcp_buf[11]));
        modbusBridgeTCP.tcp_transaction_id = (uint16_t)((((uint16_t)modbusBridgeTCP.tcp_buf[0]) << 8) | ((uint16_t)modbusBridgeTCP.tcp_buf[1]));

        AddLog(LOG_LEVEL_DEBUG_MORE, PSTR("MBS: MBRTCP to Modbus Transactionid:%d, deviceAddress:%d, functionCode:%d, startAddress:%d, Count:%d"),
               modbusBridgeTCP.tcp_transaction_id, mbdeviceaddress, mbfunctioncode, mbstartaddress, modbusBridge.dataCount);

        tasmotaModbus->Send(mbdeviceaddress, mbfunctioncode, mbstartaddress, modbusBridge.dataCount);
      }
    }
    yield(); // avoid WDT if heavy traffic
  } while (busy);
}
#endif

/*********************************************************************************************\
 * Commands
\*********************************************************************************************/

void CmndModbusBridgeSend(void)
{
  uint16_t *writeData = NULL;
  uint8_t writeDataSize = 0;
  ModbusBridgeError errorcode = ModbusBridgeError::noerror;

  JsonParser parser(XdrvMailbox.data);
  JsonParserObject root = parser.getRootObject();
  if (!root)
    return;

  modbusBridge.deviceAddress = root.getUInt(PSTR(D_JSON_MODBUS_DEVICE_ADDRESS), 0);
  uint8_t functionCode = root.getUInt(PSTR(D_JSON_MODBUS_FUNCTION_CODE), 0);
  modbusBridge.startAddress = root.getULong(PSTR(D_JSON_MODBUS_START_ADDRESS), 0);

  const char *stype = root.getStr(PSTR(D_JSON_MODBUS_TYPE), "uint8");
  modbusBridge.count = root.getUInt(PSTR(D_JSON_MODBUS_COUNT), 1);

  if (modbusBridge.deviceAddress == 0)
    errorcode = ModbusBridgeError::wrongdeviceaddress;
  else if ((functionCode > (uint8_t)ModbusBridgeFunctionCode::mb_writeSingleRegister) &&
           (functionCode != (uint8_t)ModbusBridgeFunctionCode::mb_writeMultipleCoils) &&
           (functionCode != (uint8_t)ModbusBridgeFunctionCode::mb_writeMultipleRegisters))
    errorcode = ModbusBridgeError::wrongfunctioncode; // Invalid function code
  else
  {
    modbusBridge.functionCode = static_cast<ModbusBridgeFunctionCode>(functionCode);
    if (modbusBridge.functionCode == ModbusBridgeFunctionCode::mb_undefined)
      errorcode = ModbusBridgeError::wrongfunctioncode;
  }

  modbusBridge.type = ModbusBridgeType::mb_undefined;
  if (strcmp(stype, "int8") == 0)
  {
    modbusBridge.type = ModbusBridgeType::mb_int8;
    modbusBridge.dataCount = ((modbusBridge.count - 1)/ 2) + 1;
  }
  else if (strcmp(stype, "int16") == 0)
  {
    modbusBridge.type = ModbusBridgeType::mb_int16;
    modbusBridge.dataCount = modbusBridge.count;
  }
  else if (strcmp(stype, "int32") == 0)
  {
    modbusBridge.type = ModbusBridgeType::mb_int32;
    modbusBridge.dataCount = 2 * modbusBridge.count;
  }
  else if ((strcmp(stype, "uint8") == 0))
  {
    modbusBridge.type = ModbusBridgeType::mb_uint8;
    modbusBridge.dataCount = ((modbusBridge.count - 1)/ 2) + 1;
  }
  else if ((strcmp(stype, "uint16") == 0) || (strcmp(stype, "") == 0)) // Default is uint16
  {
    modbusBridge.type = ModbusBridgeType::mb_uint16;
    modbusBridge.dataCount = modbusBridge.count;
  }
  else if (strcmp(stype, "uint32") == 0)
  {
    modbusBridge.type = ModbusBridgeType::mb_uint32;
    modbusBridge.dataCount = 2 * modbusBridge.count;
  }
  else if (strcmp(stype, "float") == 0)
  {
    modbusBridge.type = ModbusBridgeType::mb_float;
    modbusBridge.dataCount = 2 * modbusBridge.count;
  }
  else if (strcmp(stype, "raw") == 0)
  {
    modbusBridge.type = ModbusBridgeType::mb_raw;
    modbusBridge.dataCount = modbusBridge.count;
  }
  else if (strcmp(stype, "hex") == 0)
  {
    modbusBridge.type = ModbusBridgeType::mb_hex;
    modbusBridge.dataCount = modbusBridge.count;
  }
  else if (strcmp(stype, "bit") == 0)
  {
    modbusBridge.type = ModbusBridgeType::mb_bit;
    modbusBridge.dataCount = ((modbusBridge.count - 1) / 16) + 1;
  }
  else
    errorcode = ModbusBridgeError::wrongtype;

  if (modbusBridge.dataCount > MBR_MAX_REGISTERS)
    errorcode = ModbusBridgeError::wrongcount;

  // If write data is specified in JSON copy it into writeData array
  JsonParserArray jsonDataArray = root[PSTR(D_JSON_MODBUS_VALUES)].getArray();
  if (jsonDataArray.isArray())
  {
    if (modbusBridge.dataCount > 40)
    {
      errorcode = ModbusBridgeError::tomanydata;
    }
    else
    {
      writeDataSize = jsonDataArray.size();

      if (modbusBridge.count != writeDataSize)
      {
        errorcode = ModbusBridgeError::wrongcount;
      }
      else
      {
        writeData = (uint16_t *)malloc(writeDataSize * 2);
        for (uint8_t jsonDataArrayPointer = 0; jsonDataArrayPointer < writeDataSize; jsonDataArrayPointer++)
        {
          switch (modbusBridge.type)
          {
            case ModbusBridgeType::mb_bit:
              {
                // Set 2 following bytes to 0
                if (jsonDataArrayPointer % 16 == 0)
                {
                  writeData[jsonDataArrayPointer/15] = 0;
                }
                // Swap low and high bytes according to modbus specification
                uint16_t bitValue = (jsonDataArray[jsonDataArrayPointer].getUInt(0) == 1) ? 1 : 0;
                uint8_t bitPointer = (jsonDataArrayPointer % 15) + 8;
                if (bitPointer > 15) bitPointer -= 16;

                writeData[jsonDataArrayPointer/15] += bitValue << bitPointer;
              }
            break;
            case ModbusBridgeType::mb_int8:
              if (jsonDataArrayPointer % 2) writeData[jsonDataArrayPointer / 2] += (int8_t)jsonDataArray[jsonDataArrayPointer].getInt(0);
              else writeData[jsonDataArrayPointer] = (int8_t)jsonDataArray[jsonDataArrayPointer / 2].getInt(0) << 8;
            break;
            case ModbusBridgeType::mb_uint8:
              if (jsonDataArrayPointer % 2) writeData[jsonDataArrayPointer / 2] += (uint8_t)jsonDataArray[jsonDataArrayPointer].getInt(0);
              else writeData[jsonDataArrayPointer / 2] = (uint8_t)jsonDataArray[jsonDataArrayPointer].getInt(0) << 8;
            break;
            case ModbusBridgeType::mb_int16:
              writeData[jsonDataArrayPointer] = (int16_t)jsonDataArray[jsonDataArrayPointer].getInt(0);
            break;
            case ModbusBridgeType::mb_uint16:
              writeData[jsonDataArrayPointer] = (uint16_t)jsonDataArray[jsonDataArrayPointer].getUInt(0);
            break;
            case ModbusBridgeType::mb_float:
              // TODO
              errorcode = ModbusBridgeError::wrongtype;
            break;
            case ModbusBridgeType::mb_int32:
              writeData[jsonDataArrayPointer++] = (int16_t)(jsonDataArray[jsonDataArrayPointer].getInt(0) >> 16);
              writeData[jsonDataArrayPointer] = (uint16_t)jsonDataArray[jsonDataArrayPointer].getInt(0);
            break;
            case ModbusBridgeType::mb_uint32:
              writeData[jsonDataArrayPointer++] = (uint16_t)(jsonDataArray[jsonDataArrayPointer].getUInt(0) >> 16);
              writeData[jsonDataArrayPointer] = (uint16_t)jsonDataArray[jsonDataArrayPointer].getUInt(0);
            break;
            case ModbusBridgeType::mb_raw:
              writeData[jsonDataArrayPointer] = (uint8_t)jsonDataArray[jsonDataArrayPointer*2].getUInt(0) << 8 + (uint8_t)jsonDataArray[(jsonDataArrayPointer*2)+1].getUInt(0);
            break;
            case ModbusBridgeType::mb_hex:
              writeData[jsonDataArrayPointer] = (uint8_t)jsonDataArray[jsonDataArrayPointer*2].getUInt(0) << 8 + (uint8_t)jsonDataArray[(jsonDataArrayPointer*2)+1].getUInt(0);
            break;
            default:
              errorcode = ModbusBridgeError::wrongtype;
            break;
          }
        }
      }
    }
  }

  // Handle errorcode and exit function when an error has occured
  if (errorcode != ModbusBridgeError::noerror)
  {
    AddLog(LOG_LEVEL_DEBUG, PSTR("MBS: MBR Send Error %u"), (uint8_t)errorcode);
    free(writeData);
    return;
  }

  // Adapt data according to modbus protocol
  for (uint8_t writeDataPointer = 0; writeDataPointer < modbusBridge.dataCount; writeDataPointer++)
  {
    // For function code 5, on and off are 0xFF00 and 0x0000, not 1 and 0      
    if (modbusBridge.functionCode == ModbusBridgeFunctionCode::mb_writeSingleCoil)
    {
      writeData[writeDataPointer] = writeData[writeDataPointer] ? 0xFF00 : 0x0000; 
    }
  }

  // If writing a single coil or single register, the register count is always 1. We also prevent writing data out of range
  if ((modbusBridge.functionCode == ModbusBridgeFunctionCode::mb_writeSingleCoil) || (modbusBridge.functionCode == ModbusBridgeFunctionCode::mb_writeSingleRegister))
    modbusBridge.dataCount = 1;

  uint8_t error = tasmotaModbus->Send(modbusBridge.deviceAddress, (uint8_t)modbusBridge.functionCode, modbusBridge.startAddress, modbusBridge.dataCount, writeData);
  if (error) AddLog(LOG_LEVEL_DEBUG, PSTR("MBS: MBR Driver send error %u"), error);
  free(writeData);
  ResponseCmndDone();
}

void CmndModbusBridgeSetBaudrate(void)
{
  SetModbusBridgeBaudrate(XdrvMailbox.payload);
  ResponseCmndNumber(Settings->modbus_sbaudrate * 300);
}

void CmndModbusBridgeSetConfig(void)
{
  // See TasmotaModusConfig for possible options
  // ModbusConfig 0..23 where 3 equals 8N1
  // ModbusConfig 8N1

  if (XdrvMailbox.data_len > 0)
  {
    if (XdrvMailbox.data_len < 3)
    { // Use 0..23 as serial config option
      if ((XdrvMailbox.payload >= TS_SERIAL_5N1) && (XdrvMailbox.payload <= TS_SERIAL_8O2))
      {
        SetModbusBridgeConfig(XdrvMailbox.payload);
      }
    }
    else if ((XdrvMailbox.payload >= 5) && (XdrvMailbox.payload <= 8))
    {
      int8_t serial_config = ParseSerialConfig(XdrvMailbox.data);
      if (serial_config >= 0)
      {
        SetModbusBridgeConfig(serial_config);
      }
    }
  }
  ResponseCmndChar(GetSerialConfig(Settings->modbus_sconfig).c_str());
}

#ifdef USE_MODBUS_BRIDGE_TCP
//
// Command `TCPStart`
// Params: port,<IPv4 allow>
//
void CmndModbusTCPStart(void)
{

  if (!tasmotaModbus)
  {
    return;
  }

  int32_t tcp_port = XdrvMailbox.payload;
  if (ArgC() == 2)
  {
    char sub_string[XdrvMailbox.data_len];
    modbusBridgeTCP.ip_filter.fromString(ArgV(sub_string, 2));
  }
  else
  {
    // Disable whitelist if previously set
    modbusBridgeTCP.ip_filter = (uint32_t)0;
  }

  if (modbusBridgeTCP.server_tcp)
  {
    AddLog(LOG_LEVEL_INFO, PSTR("MBS: MBRTCP Stopping server"));
    modbusBridgeTCP.server_tcp->stop();
    delete modbusBridgeTCP.server_tcp;
    modbusBridgeTCP.server_tcp = nullptr;

    for (uint32_t i = 0; i < nitems(modbusBridgeTCP.client_tcp); i++)
    {
      WiFiClient &client = modbusBridgeTCP.client_tcp[i];
      client.stop();
    }
  }
  if (tcp_port > 0)
  {
    AddLog(LOG_LEVEL_INFO, PSTR("MBS: MBRTCP Starting server on port %d"), tcp_port);
    if (modbusBridgeTCP.ip_filter)
    {
      AddLog(LOG_LEVEL_INFO, PSTR("MBS: MBRTCP Filtering %s"), modbusBridgeTCP.ip_filter.toString().c_str());
    }
    modbusBridgeTCP.server_tcp = new WiFiServer(tcp_port);
    modbusBridgeTCP.server_tcp->begin(); // start TCP server
    modbusBridgeTCP.server_tcp->setNoDelay(true);
  }

  ResponseCmndDone();
}

//
// Command `Connect`
// Params: port,<IPv4>
//
void CmndModbusTCPConnect(void)
{
  int32_t tcp_port = XdrvMailbox.payload;

  if (!tasmotaModbus)
  {
    return;
  }

  if (ArgC() == 2)
  {
    char sub_string[XdrvMailbox.data_len];
    WiFiClient new_client;
    AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_TCP "MBS: MBRTCP Connecting to %s on port %d"), ArgV(sub_string, 2), tcp_port);
    if (new_client.connect(ArgV(sub_string, 2), tcp_port))
    {
      AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_TCP "MBS: MBRTCP connected!"));
    }
    else
    {
      AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_TCP "MBS: MBRTCP error connecting!"));
    }

    // find an empty slot
    uint32_t i;
    for (i = 0; i < nitems(modbusBridgeTCP.client_tcp); i++)
    {
      WiFiClient &client = modbusBridgeTCP.client_tcp[i];
      if (!client)
      {
        client = new_client;
        break;
      }
    }
    if (i >= nitems(modbusBridgeTCP.client_tcp))
    {
      i = modbusBridgeTCP.client_next++ % nitems(modbusBridgeTCP.client_tcp);
      WiFiClient &client = modbusBridgeTCP.client_tcp[i];
      client.stop();
      client = new_client;
    }
  }
  else
  {
    AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_TCP "MBS: MBR Usage: port,ip_address"));
  }

  ResponseCmndDone();
}
#endif

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv63(uint8_t function)
{
  bool result = false;

  if (FUNC_PRE_INIT == function)
  {
    ModbusBridgeInit();
  }
  else if (tasmotaModbus)
  {
    switch (function)
    {
    case FUNC_COMMAND:
      result = DecodeCommand(kModbusBridgeCommands, ModbusBridgeCommand);
      break;
    case FUNC_LOOP:
      ModbusBridgeHandle();
#ifdef USE_MODBUS_BRIDGE_TCP
      ModbusTCPHandle();
#endif
      break;
    }
  }
  return result;
}

#endif // USE_MODBUS_BRIDGE
