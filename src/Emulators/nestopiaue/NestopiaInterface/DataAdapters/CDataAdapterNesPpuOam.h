#ifndef _CDataAdapterNesPpuOam_H_
#define _CDataAdapterNesPpuOam_H_

#include "CDataAdapter.h"

class CDebugInterfaceNes;

class CDataAdapterNesPpuOam : public CDataAdapter
{
public:
	CDataAdapterNesPpuOam(CDebugInterfaceNes *debugInterfaceNes);
	CDebugInterfaceNes *debugInterfaceNes;
	
	virtual int AdapterGetDataLength();
	virtual void AdapterReadByte(int pointer, uint8 *value);
	virtual void AdapterWriteByte(int pointer, uint8 value);
	virtual void AdapterReadByte(int pointer, uint8 *value, bool *isAvailable);
	virtual void AdapterWriteByte(int pointer, uint8 value, bool *isAvailable);
	virtual void AdapterReadBlockDirect(uint8 *buffer, int pointerStart, int pointerEnd);
};



#endif

