#include "CDebugMemoryMapCell.h"
#include "CDebugMemoryMap.h"

///
void _markCellReadStyleDebugger(CDebugMemoryMapCell *cell)
{
	cell->sg = 0.3f;
	cell->sb = 1.0f;
	cell->sa = 1.0f;
}

void _markCellWriteStyleDebugger(CDebugMemoryMapCell *cell)
{
	cell->sr = 1.0f;
	cell->sa = 1.0f;
}

void _markCellExecuteCodeStyleDebugger(CDebugMemoryMapCell *cell)
{
	cell->sr = 0.0f;
	cell->sg = 1.0f;
	cell->sb = 1.0f;
	cell->sa = 1.0f;
}

void _markCellExecuteArgumentStyleDebugger(CDebugMemoryMapCell *cell)
{
	cell->sr = 0.0f;
	cell->sg = 0.3f;
	cell->sb = 1.0f;
	cell->sa = 1.0f;
}

void _markCellReadStyleICU(CDebugMemoryMapCell *cell)
{
	cell->sg = 1.0f;
	cell->sa = 1.0f;
}

void _markCellWriteStyleICU(CDebugMemoryMapCell *cell)
{
	cell->sr = 1.0f;
	cell->sa = 1.0f;
}

void _markCellExecuteCodeStyleICU(CDebugMemoryMapCell *cell)
{
	cell->sr = 0.0f;
	cell->sg = 1.0f;
	cell->sb = 1.0f;
	cell->sa = 1.0f;
}

void _markCellExecuteArgumentStyleICU(CDebugMemoryMapCell *cell)
{
	cell->sr = 0.0f;
	cell->sg = 1.0f;
	cell->sb = 0.0f;
	cell->sa = 1.0f;
}

typedef void (*MarkCellReadFunc)(CDebugMemoryMapCell *cell);
typedef void (*MarkCellWriteFunc)(CDebugMemoryMapCell *cell);
typedef void (*MarkCellExecuteCodeFunc)(CDebugMemoryMapCell *cell);
typedef void (*MarkCellExecuteArgumentFunc)(CDebugMemoryMapCell *cell);

MarkCellReadFunc markCellRead = _markCellReadStyleDebugger;
MarkCellWriteFunc markCellWrite = _markCellWriteStyleDebugger;
MarkCellExecuteCodeFunc markCellExecuteCode = _markCellExecuteCodeStyleDebugger;
MarkCellExecuteArgumentFunc markCellExecuteArgument = _markCellExecuteArgumentStyleDebugger;

void C64DebuggerSetMemoryMapMarkersStyle(uint8 memoryMapMarkersStyle)
{
	float darken = 0.5f;
	if (memoryMapMarkersStyle == MEMORY_MAP_MARKER_STYLE_DEFAULT)
	{
		markCellRead = _markCellReadStyleDebugger;
		markCellWrite = _markCellWriteStyleDebugger;
		markCellExecuteCode = _markCellExecuteCodeStyleDebugger;
		markCellExecuteArgument = _markCellExecuteArgumentStyleDebugger;
		
		colorExecuteCodeR = 0.0f * darken;
		colorExecuteCodeG = 1.0f * darken;
		colorExecuteCodeB = 1.0f * darken;
		colorExecuteCodeA = 1.0f;
		
		colorExecuteArgumentR = 0.0f * darken;
		colorExecuteArgumentG = 0.0f * darken;
		colorExecuteArgumentB = 1.0f * darken;
		colorExecuteArgumentA = 1.0f;

	}
	else if (memoryMapMarkersStyle == MEMORY_MAP_MARKER_STYLE_ICU)
	{
		markCellRead = _markCellReadStyleICU;
		markCellWrite = _markCellWriteStyleICU;
		markCellExecuteCode = _markCellExecuteCodeStyleICU;
		markCellExecuteArgument = _markCellExecuteArgumentStyleICU;

		colorExecuteCodeR = 0.0f * darken;
		colorExecuteCodeG = 1.0f * darken;
		colorExecuteCodeB = 1.0f * darken;
		colorExecuteCodeA = 1.0f;
		
		colorExecuteArgumentR = 0.0f * darken;
		colorExecuteArgumentG = 1.0f * darken;
		colorExecuteArgumentB = 0.0f * darken;
		colorExecuteArgumentA = 1.0f;
	}
	
	float fce = 0.65f;
	colorExecuteCodeR2 = colorExecuteCodeR * fce;
	colorExecuteCodeG2 = colorExecuteCodeG * fce;
	colorExecuteCodeB2 = colorExecuteCodeB * fce;
	colorExecuteCodeA2 = 1.0f; //colorExecuteCodeA * fce;
	
	colorExecuteArgumentR2 = colorExecuteArgumentR * fce;
	colorExecuteArgumentG2 = colorExecuteArgumentG * fce;
	colorExecuteArgumentB2 = colorExecuteArgumentB * fce;
	colorExecuteArgumentA2 = 1.0f; //colorExecuteArgumentA * fce;

}


CDebugMemoryMapCell::CDebugMemoryMapCell(int addr)
{
	this->addr = addr;
	
	sr = 0.0f; vr = 0.0f; rr = 0.0f;
	sg = 0.0f; vg = 0.0f; rg = 0.0f;
	sb = 0.0f; vb = 0.0f; rb = 0.0f;
	sa = 0.0f; va = 0.0f; ra = 0.0f;
	
	isExecuteCode = false;
	isExecuteArgument = false;
	
	isRead = false;
	isWrite = false;

	writePC = writeRasterCycle = writeRasterLine = -1;
	readPC  = readRasterCycle  = readRasterLine = -1;
	writeCycle = readCycle = executeCycle = -1;
}

CDebugMemoryMapCell::~CDebugMemoryMapCell()
{
}

void CDebugMemoryMapCell::ClearExecuteMarkers()
{
	isExecuteCode = false;
	isExecuteArgument = false;
	isRead = false;
	isWrite = false;

	sr = 0.0f; vr = 0.0f; rr = 0.0f;
	sg = 0.0f; vg = 0.0f; rg = 0.0f;
	sb = 0.0f; vb = 0.0f; rb = 0.0f;
	sa = 0.0f; va = 0.0f; ra = 0.0f;
}

void CDebugMemoryMapCell::ClearReadWriteMarkers()
{
	isRead = false;
	isWrite = false;

	sr = 0.0f; vr = 0.0f; rr = 0.0f;
	sg = 0.0f; vg = 0.0f; rg = 0.0f;
	sb = 0.0f; vb = 0.0f; rb = 0.0f;
	sa = 0.0f; va = 0.0f; ra = 0.0f;
	
	writePC = writeRasterCycle = writeRasterLine = -1;
	readPC  = readRasterCycle  = readRasterLine = -1;
	writeCycle = readCycle = executeCycle = -1;
}

void CDebugMemoryMapCell::MarkCellRead()
{
	markCellRead(this);
	isRead = true;
}

void CDebugMemoryMapCell::MarkCellWrite(uint8 value)
{
	//LOGTODO("remove argument marker based on previous code length");
	
	isExecuteCode = false;
	isExecuteArgument = false;
	isWrite = true;
	markCellWrite(this);
}

void CDebugMemoryMapCell::MarkCellExecuteCode(uint8 opcode)
{
	isExecuteCode = true;
	markCellExecuteCode(this);
}

void CDebugMemoryMapCell::MarkCellExecuteArgument()
{
	isExecuteArgument = true;
	markCellExecuteArgument(this);
}
