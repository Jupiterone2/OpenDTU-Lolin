#include "HoymilesRadio.h"
#include "Hoymiles.h"
#include "crc.h"
#include <Every.h>
#include <FunctionalInterrupt.h>

void HoymilesRadio::init()
{
    _dtuSerial.u64 = 0;

    _radio.reset(new RF24(4, 5));
    _radio->begin();
    _radio->setDataRate(RF24_250KBPS);
    _radio->enableDynamicPayloads();
    _radio->setCRCLength(RF24_CRC_16);
    _radio->setAddressWidth(5);
    _radio->setRetries(0, 0);
    _radio->maskIRQ(true, true, false); // enable only receiving interrupts
    if (_radio->isChipConnected()) {
        Serial.println(F("Connection successfull"));
    } else {
        Serial.println(F("Connection error!!"));
    }

    attachInterrupt(digitalPinToInterrupt(16), std::bind(&HoymilesRadio::handleIntr, this), FALLING);

    openReadingPipe();
    _radio->startListening();
}

void HoymilesRadio::loop()
{
    EVERY_N_MILLIS(4)
    {
        switchRxCh();
    }

    if (_packetReceived) {
        Serial.println(F("Interrupt received"));
        while (_radio->available()) {
            if (!_rxBuffer.full()) {
                fragment_t* f;
                f = _rxBuffer.getFront();
                memset(f->fragment, 0xcc, MAX_RF_PAYLOAD_SIZE);
                f->len = _radio->getDynamicPayloadSize();
                if (f->len > MAX_RF_PAYLOAD_SIZE)
                    f->len = MAX_RF_PAYLOAD_SIZE;

                _radio->read(f->fragment, f->len);
                _rxBuffer.pushFront(f);
            } else {
                Serial.println(F("Buffer full"));
                _radio->flush_rx();
            }
        }
        _packetReceived = false;

    } else {
        // Perform package parsing only if no packages are received
        if (!_rxBuffer.empty()) {
            fragment_t* f = _rxBuffer.getBack();
            if (checkFragmentCrc(f)) {
                std::shared_ptr<InverterAbstract> inv = Hoymiles.getInverterByFragment(f);

                if (nullptr != inv) {
                    // Save packet in inverter rx buffer
                    dumpBuf("RX ", f->fragment, f->len);
                    inv->addRxFragment(f->fragment, f->len);
                } else {
                    Serial.println(F("Inverter Not found!"));
                }

            } else {
                Serial.println(F("Frame kaputt"));
            }

            // Remove paket from buffer even it was corrupted
            _rxBuffer.popBack();
        }
    }

    if (_busyFlag && _rxTimeout.occured()) {
        Serial.println(F("RX Period End"));
        std::shared_ptr<InverterAbstract> inv = Hoymiles.getInverterBySerial(_activeSerial.u64);

        if (nullptr != inv) {
            uint8_t verifyResult = inv->verifyAllFragments();
            if (verifyResult == 255) {
                Serial.println(F("Should Retransmit whole thing"));
                // todo: irgendwas tun wenn garnichts ankam....
                _busyFlag = false;

            } else if (verifyResult == 254) {
                Serial.println(F("Retransmit timeout"));
                _busyFlag = false;

            } else if (verifyResult == 253) {
                Serial.println(F("Packet CRC error"));
                _busyFlag = false;

            } else if (verifyResult > 0) {
                // Perform Retransmit
                Serial.print(F("Request retransmit: "));
                Serial.println(verifyResult);
                sendRetransmitPacket(verifyResult);

            } else {
                // Successfull received all packages
                Serial.println(F("Success"));
                _busyFlag = false;
            }
        }
    }
}

void HoymilesRadio::setPALevel(rf24_pa_dbm_e paLevel)
{
    _radio->setPALevel(paLevel);
}

serial_u HoymilesRadio::DtuSerial()
{
    return _dtuSerial;
}

void HoymilesRadio::setDtuSerial(uint64_t serial)
{
    _dtuSerial.u64 = serial;
    openReadingPipe();
}

bool HoymilesRadio::isIdle()
{
    return !_busyFlag;
}

void HoymilesRadio::openReadingPipe()
{
    serial_u s;
    s = convertSerialToRadioId(_dtuSerial);
    _radio->openReadingPipe(1, s.u64);
}

void HoymilesRadio::openWritingPipe(serial_u serial)
{
    serial_u s;
    s = convertSerialToRadioId(serial);
    _radio->openWritingPipe(s.u64);
}

void ARDUINO_ISR_ATTR HoymilesRadio::handleIntr()
{
    _packetReceived = true;
}

uint8_t HoymilesRadio::getRxNxtChannel()
{
    if (++_rxChIdx >= 4)
        _rxChIdx = 0;
    return _rxChLst[_rxChIdx];
}

uint8_t HoymilesRadio::getTxNxtChannel()
{
    if (++_txChIdx >= 1)
        _txChIdx = 0;
    return _txChLst[_txChIdx];
}

void HoymilesRadio::switchRxCh()
{

    // portDISABLE_INTERRUPTS();
    _radio->stopListening();
    _radio->setChannel(getRxNxtChannel());
    _radio->startListening();
    // portENABLE_INTERRUPTS();
}

serial_u HoymilesRadio::convertSerialToRadioId(serial_u serial)
{
    serial_u radioId;
    radioId.u64 = 0;
    radioId.b[4] = serial.b[0];
    radioId.b[3] = serial.b[1];
    radioId.b[2] = serial.b[2];
    radioId.b[1] = serial.b[3];
    radioId.b[0] = 0x01;
    return radioId;
}

void HoymilesRadio::convertSerialToPacketId(uint8_t buffer[], serial_u serial)
{
    buffer[3] = serial.b[0];
    buffer[2] = serial.b[1];
    buffer[1] = serial.b[2];
    buffer[0] = serial.b[3];
}

bool HoymilesRadio::checkFragmentCrc(fragment_t* fragment)
{
    uint8_t crc = crc8(fragment->fragment, fragment->len - 1);
    return (crc == fragment->fragment[fragment->len - 1]);
}

void HoymilesRadio::sendEsbPacket(serial_u target, uint8_t mainCmd, uint8_t subCmd, uint8_t payload[], uint8_t len, uint32_t timeout, bool resend)
{
    static uint8_t txBuffer[MAX_RF_PAYLOAD_SIZE];

    if (!resend) {
        memset(txBuffer, 0, MAX_RF_PAYLOAD_SIZE);

        txBuffer[0] = mainCmd;
        convertSerialToPacketId(&txBuffer[1], target); // 4 byte long
        convertSerialToPacketId(&txBuffer[5], DtuSerial()); // 4 byte long
        txBuffer[9] = subCmd;

        memcpy(&txBuffer[10], payload, len);
        txBuffer[10 + len] = crc8(txBuffer, 10 + len);
    }

    _radio->stopListening();
    _radio->setChannel(getTxNxtChannel());
    openWritingPipe(target);
    _radio->setRetries(3, 15);

    dumpBuf("TX ", txBuffer, 10 + len + 1);
    _radio->write(txBuffer, 10 + len + 1);

    _radio->setRetries(0, 0);
    openReadingPipe();
    _radio->setChannel(getRxNxtChannel());
    _radio->startListening();
    _busyFlag = true;
    _rxTimeout.set(timeout);
}

void HoymilesRadio::sendTimePacket(std::shared_ptr<InverterAbstract> iv, time_t ts)
{
    uint8_t payload[16] = { 0 };

    payload[0] = 0x0b;
    payload[1] = 0x00;
    u32CpyLittleEndian(&payload[2], ts); // sets the 4 following elements {2, 3, 4, 5}
    payload[9] = 0x05;

    uint16_t crc = crc16(&payload[0], 14);
    payload[14] = (crc >> 8) & 0xff;
    payload[15] = (crc)&0xff;

    serial_u s;
    s.u64 = iv->serial();
    _activeSerial.u64 = iv->serial();

    sendEsbPacket(s, 0x15, 0x80, payload, sizeof(payload) / sizeof(uint8_t), 200);
}

void HoymilesRadio::sendRetransmitPacket(uint8_t fragment_id)
{
    sendEsbPacket(_activeSerial, 0x15, (uint8_t)(0x80 + fragment_id), 0, 0, 60);
}

void HoymilesRadio::sendLastPacketAgain()
{
    sendEsbPacket(_activeSerial, 0, 0, 0, 0, 60, true);
}

void HoymilesRadio::u32CpyLittleEndian(uint8_t dest[], uint32_t src)
{
    dest[0] = ((src >> 24) & 0xff);
    dest[1] = ((src >> 16) & 0xff);
    dest[2] = ((src >> 8) & 0xff);
    dest[3] = ((src)&0xff);
}

void HoymilesRadio::dumpBuf(const char* info, uint8_t buf[], uint8_t len)
{

    if (NULL != info)
        Serial.print(String(info));

    for (uint8_t i = 0; i < len; i++) {
        Serial.print(buf[i], 16);
        Serial.print(" ");
    }
    Serial.println(F(""));
}