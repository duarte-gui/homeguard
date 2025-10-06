/*
	Wiegand.cpp - Library for reading Wiegand protocol data.
	Created by R. Zschiegner, 2011-2021
	Released into the public domain.

    (Adaptado para ESP32 e projeto Homeguard)
*/
#include "Arduino.h" // CORREÇÃO CRÍTICA: Inclui todas as funções e tipos do Arduino
#include "Wiegand.h" // Deve corresponder ao nome do arquivo de cabeçalho renomeado

// Static member initializations
volatile uint32_t Wiegand::_cardTemp = 0;
volatile uint32_t Wiegand::_lastWiegand = 0;
volatile uint8_t Wiegand::_bitCount = 0;
uint8_t Wiegand::_pinD0 = 0;
uint8_t Wiegand::_pinD1 = 0;

Wiegand::Wiegand() {
    _code = 0;
    _wiegandType = 0;
}

uint32_t Wiegand::getCode() {
	return _code;
}

uint8_t Wiegand::getWiegandType() {
	return _wiegandType;
}

bool Wiegand::available() {
    bool ret = false;
    noInterrupts(); // ATOMIC SECTION START
    if ((millis() - _lastWiegand) > WIEGAND_WAIT_TIME && _bitCount > 0) {
        if (_bitCount == 4) { // Teclado 4-bit
            _wiegandType = 4;
            _code = (_cardTemp & 0x0F);
        }
        else if (_bitCount == 26) { // Cartão 26-bit
            _wiegandType = 26;
            _code = (_cardTemp >> 1) & 0x00FFFFFF;
        } else { // Outros formatos
            _wiegandType = _bitCount;
            _code = _cardTemp;
        }
        ret = true;
        _bitCount = 0;
        _cardTemp = 0;
    }
    interrupts(); // ATOMIC SECTION END
	return ret;
}

void Wiegand::flush(){
    noInterrupts();
    _lastWiegand=0;
    _cardTemp=0;
    _bitCount=0;
    interrupts();
}

void IRAM_ATTR Wiegand::ReadD0() {
	_bitCount++;
	_cardTemp = _cardTemp << 1;
	_lastWiegand = millis();
}

void IRAM_ATTR Wiegand::ReadD1() {
	_bitCount++;
	_cardTemp = _cardTemp << 1;
	_cardTemp |= 1;
	_lastWiegand = millis();
}

void Wiegand::begin(uint8_t pinD0, uint8_t pinD1) {
    _pinD0 = pinD0;
    _pinD1 = pinD1;
    
    pinMode(_pinD0, INPUT);
	pinMode(_pinD1, INPUT);
    
    attachInterrupt(digitalPinToInterrupt(_pinD0), ReadD0, FALLING);
	attachInterrupt(digitalPinToInterrupt(_pinD1), ReadD1, FALLING);
	
    _lastWiegand = 0;
	_cardTemp = 0;
	_bitCount = 0;
}
