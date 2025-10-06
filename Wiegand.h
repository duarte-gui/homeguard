/*
	Wiegand.h - Library for reading Wiegand protocol data.
	Created by R. Zschiegner, 2011-2021
	Released into the public domain.
    
    (Adaptado para ESP32 e projeto Homeguard)
*/
#ifndef Wiegand_h
#define Wiegand_h

#include "Arduino.h" // CORREÇÃO: Essencial para tipos como uint8_t e uint32_t

class Wiegand {
	public:
		Wiegand();
		void begin(uint8_t pinD0, uint8_t pinD1);
		bool available();
		uint32_t getCode();
		uint8_t getWiegandType();
        void flush();

	private:
        static void ReadD0();
		static void ReadD1();
		static volatile uint32_t _cardTemp;
		static volatile uint32_t _lastWiegand;
		static volatile uint8_t _bitCount;
		static const uint8_t WIEGAND_WAIT_TIME = 100;
        static uint8_t _pinD0;
        static uint8_t _pinD1;
        
        uint32_t _code;
        uint8_t _wiegandType;
};

#endif
