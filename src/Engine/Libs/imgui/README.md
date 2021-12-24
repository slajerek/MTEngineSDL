# ImGui notes

This is ImGui docking unstable branch which is heavily developed. There are a few changes required however.

## ImGuiWindow 

ImGuiWindow is extended with `void *userData` to allow assigning CGuiView to an ImGuiWindow.

### imgui_internal.h

```
// Storage for one window
struct IMGUI_API ImGuiWindow
{
```

userData is added:

```
    // any user data assigned to this ImGuiWindow
    void *userData;
```

## Fix for DPI

ImGui does not support multiple monitor DPIs. The patch is required to allow different DPIs on 2 monitors. 
Example use case is a Macbook Pro (high dpi) connected to a standard external HD monitor (low dpi). 
This bug causes that all viewports contents on external monitor are rendered scaled 2x (bigger). 
Also current SDL implementation of SDL_GetDisplayDPI gives wrong results on macOS and thus a workaround function is called.

This hack below works, but is not recommended way of fixing the problem and may cause problems in the future.

### imgui_impl_sdl.cpp

```
float MACOS_GetBackingScaleFactor(int screen);

#if __APPLE__
                monitor.DpiScale = MACOS_GetBackingScaleFactor(n);
#elif SDL_HAS_PER_MONITOR_DPI
        float dpi = 0.0f;
        if (!SDL_GetDisplayDPI(n, &dpi, NULL, NULL))
            monitor.DpiScale = dpi / 96.0f;
#endif
        platform_io.Monitors.push_back(monitor);
```        
        
### imgui.cpp

```
	//draw_data->FramebufferScale = io.DisplayFramebufferScale; // FIXME-VIEWPORT: This may vary on a per-monitor/viewport basis?
	ImGuiContext& g = *GImGui;
	if (g.ConfigFlagsCurrFrame & ImGuiConfigFlags_ViewportsEnable)
				draw_data->FramebufferScale = ImVec2(viewport->DpiScale, viewport->DpiScale);
			else
				draw_data->FramebufferScale = io.DisplayFramebufferScale;
```

# Notes

DPI fix patch is being discussed here: 
https://github.com/ocornut/imgui/commit/a843af4306e0d786fec5394bba07fd5067384661

Correct way to approach this bug : 
https://github.com/ocornut/imgui/blob/master/docs/FAQ.md#q-how-should-i-handle-dpi-in-my-application
