#ifndef _CGUIVIEWTERMINAL_H_
#define _CGUIVIEWTERMINAL_H_

#include "CGuiView.h"
#include <functional>
#include <mutex>
#include <cstdint>

// Forward declare libtmt types
struct TMT;

class CGuiViewTerminal : public CGuiView
{
public:
	CGuiViewTerminal(const char *name, float posX, float posY, float posZ,
					 float sizeX, float sizeY, int cols = 80, int rows = 25);
	virtual ~CGuiViewTerminal();

	virtual void RenderImGui();
	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyTextInput(const char *text);

	// Feed raw bytes from network/transport into the terminal emulator
	void ProcessInput(const uint8_t *data, size_t len);

	// Set callback for data that needs to be sent back (keystrokes, terminal responses)
	void SetWriteCallback(std::function<void(const uint8_t*, size_t)> callback);

	// Get terminal dimensions
	int GetCols() const { return cols; }
	int GetRows() const { return rows; }

	// Send data via write callback (public so libtmt C callback can access it)
	void SendData(const char *data);
	void SendData(const uint8_t *data, size_t len);

protected:
	TMT *vt;
	int cols;
	int rows;
	std::mutex vtMutex;
	std::function<void(const uint8_t*, size_t)> writeCallback;

	// Cursor blink
	float cursorBlinkTimer;
	bool cursorVisible;

	// Color table: [0-7] normal, [8-15] bright
	unsigned int colorTable[16];

	// Map MTEngineSDL key codes to VT-100 sequences
	const char *MapKeyToVT100(u32 keyCode, bool isShift, bool isAlt, bool isControl);
};

#endif
