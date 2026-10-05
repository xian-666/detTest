#pragma once
#include <stdint.h>

class IDevice {
public:
	virtual bool Open() = 0;
	virtual bool Close() = 0;
	virtual bool Write(unsigned char* bytes, int size) = 0;
};