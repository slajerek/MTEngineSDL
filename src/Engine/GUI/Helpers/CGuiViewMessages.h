#ifndef _CGuiViewMessages_h_
#define _CGuiViewMessages_h_

#include "CGuiView.h"
#include <string>
#include <type_traits>

class CSlrMutex;

class CGuiViewMessages : public CGuiView
{
public:
	CGuiViewMessages(const char *name, float posX, float posY, float sizeX, float sizeY);
	CGuiViewMessages(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewMessages();
	
	virtual void Render();
	virtual void Render(float posX, float posY);
	virtual void RenderImGui();
	virtual void DoLogic();
	
	virtual bool DoTap(float x, float y);
	virtual bool DoFinishTap(float x, float y);
	
	virtual bool DoDoubleTap(float x, float y);
	virtual bool DoFinishDoubleTap(float posX, float posY);
	
	virtual bool DoMove(float x, float y, float distX, float distY, float diffX, float diffY);
	virtual bool FinishMove(float x, float y, float distX, float distY, float accelerationX, float accelerationY);
	
	virtual bool InitZoom();
	virtual bool DoZoomBy(float x, float y, float zoomValue, float difference);
	
	// multi touch
	virtual bool DoMultiTap(COneTouchData *touch, float x, float y);
	virtual bool DoMultiMove(COneTouchData *touch, float x, float y);
	virtual bool DoMultiFinishTap(COneTouchData *touch, float x, float y);
	
	virtual bool KeyDown(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyDownRepeat(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyUp(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);
	virtual bool KeyPressed(u32 keyCode, bool isShift, bool isAlt, bool isControl, bool isSuper);	// repeats
	
	virtual void ActivateView();
	virtual void DeactivateView();
	
	virtual void Run();
	
	// based on ExampleAppLog
	
	ImGuiTextBuffer     Buf;
	ImGuiTextFilter     Filter;
	ImVector<int>       LineOffsets; // Index to lines offset. We maintain this with AddLog() calls.
	bool                AutoScroll;  // Keep scrolling if already at the bottom.
	
	void Clear();

	// AddLog supports both C strings and std::string arguments with %s
	template<typename... Args>
	void AddLog(const char* fmt, Args&&... args)
	{
		_AddLogImpl(fmt, _addLogArg(static_cast<Args&&>(args))...);
	}

	CSlrMutex *mutex;

private:
	void _AddLogImpl(const char* fmt, ...) IM_FMTARGS(2);

	static inline const char* _addLogArg(const std::string& s) { return s.c_str(); }
	template<typename T>
	static inline typename std::enable_if<!std::is_same<typename std::decay<T>::type, std::string>::value, T&&>::type
	_addLogArg(T&& arg) { return static_cast<T&&>(arg); }
};

#endif //_VIEW_C64GOATTRACKER_
