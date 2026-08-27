# llama.cpp Integration (All Platforms)

This repo vendors upstream `llama.cpp` as a git submodule at:

- `other/lib/llama.cpp`

Engine wrapper code lives at:

- `src/Engine/Sci/Llama/`

## Repo setup

1. Clone normally.
2. Initialize submodules:

```bash
git submodule update --init --recursive
```

If `other/lib/llama.cpp` is empty, you skipped this step.

## Build switches

- `MT_ENABLE_LLAMA_CPP` (default ON)
  - ON when undefined or `=1`
  - OFF when `=0`

Linux (CMake only):

- `MT_LLAMA_CUDA=ON` enables CUDA backend
- `MT_LLAMA_VULKAN=ON` enables Vulkan backend

Windows:

- CPU backend is linked into the engine as `llama_cpp.lib`.
- CUDA backend is an optional runtime plugin DLL (`mt_llama_cuda_backend.dll`). If missing, the engine falls back to CPU.

## CGuiViewLlamaModelLoader

`src/Engine/GUI/Helpers/CGuiViewLlamaModelLoader` — ImGui view for selecting, downloading, and loading a llama model. Two construction modes:

**Self-contained** (view creates and owns its own service and model manager):
```cpp
auto *view = new CGuiViewLlamaModelLoader("LlamaLoader", x, y, z, w, h);
```

**Externally-provided** (caller owns `CLlamaService`, optionally `CLlamaModelManager`):
```cpp
// LightHeroes example — service lives in CLlamaService member of the app
CLlamaService *svc = myApp->llamaService;
auto *view = new CGuiViewLlamaModelLoader("LlamaLoader", x, y, z, w, h,
                                          svc /*, optionalModelManager */);
```
The view does **not** delete externally-provided instances. If `externalModelManager` is `nullptr` the view creates its own `CLlamaModelManager`. `CLlamaModelDownloader` is always owned by the view.

## Async Text Generation

`CLlamaService` exposes four inference methods forwarded to the active backend:

```cpp
// Start async generation. Returns false if already generating or no model loaded.
// tokenCb: called from background thread for each streamed token piece (may be nullptr).
// doneCb:  called from background thread when generation finishes (may be nullptr).
// Use max_tokens = 0 to only process the prompt (KV cache rehydration), no sampling.
bool GenerateAsync(const std::string &prompt, const MT_LlamaGenerateParams &params,
                   MT_LlamaTokenCallback tokenCb, std::function<void()> doneCb);

bool IsGenerating() const;

// Signal stop and block until the inference thread exits.
void StopGeneration();

// Clear the KV cache and reset the incremental context counter.
// Do not call while generating.
void ClearContext();
```

`MT_LlamaGenerateParams` fields:
- `max_tokens` (default 512) — max tokens to sample; 0 = prompt-only (no sampling)
- `seed` (default 0xFFFFFFFF) — sampler seed
- `temperature` (default 0.8f) — sampling temperature

The `LLamaBackend_LlamaCpp` backend tracks `n_past` (tokens in the KV cache). Each `GenerateAsync` call tokenises the **full** prompt and only decodes the suffix beyond `n_past`, enabling incremental conversation context without re-processing prior turns.

## CGuiViewLlamaChat

`src/Engine/GUI/Helpers/CGuiViewLlamaChat` — ImGui chat view with streaming token display.

Constructor (non-owning service pointer):
```cpp
auto *chat = new CGuiViewLlamaChat("AI Chat", x, y, z, w, h, llamaServicePtr);
```

**Layout:**
- Scrollable history: user messages in right-aligned colored bubbles (starting at 50% width); assistant responses left-aligned with "Thought for Xs" timing header
- Sticky input bar: text field + Send button (or Stop while generating) + Enter key shortcut
- Bottom row: **New Context** (clears history + KV cache), **Save...** / **Load...** (JSON file path popup)

**Save / Load format** (`committedContext` + `entries` array):
```json
{
  "committedContext": "User: hello\nAssistant: Hi!\n",
  "entries": [
    {"isUser": true, "text": "hello"},
    {"isUser": false, "text": "Hi!", "thinkingTimeSec": 1.3}
  ]
}
```
On load, `committedContext` is replayed via `GenerateAsync(..., {max_tokens=0}, ...)` to rehydrate the KV cache.

## Platform docs

- macOS: `docs/llama_cpp_macos.md`
- Windows (CPU + CUDA plugin): `docs/llama_cpp_windows.md`
- Linux (CPU/CUDA/Vulkan): `docs/llama_cpp_linux.md`
