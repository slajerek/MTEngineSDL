#ifndef _imguiComboFilter_h_
#define _imguiComboFilter_h_

#include "imgui.h"

namespace ImGui
{
	struct ComboFilterState
	{
		int  activeIdx;
		bool selectionChanged;
	};

	class ComboFilterCallback
	{
	public:
		virtual bool ComboFilterShouldOpenPopupCallback(const char *label, char *buffer, int bufferlen,
														const char **hints, int num_hints, ImGui::ComboFilterState *s);
	};

	bool ComboFilter(const char *label, char *buffer, int bufferlen, const char **hints, int num_hints, ComboFilterState &s, ComboFilterCallback *callback, ImGuiComboFlags flags = 0);

}

#endif

