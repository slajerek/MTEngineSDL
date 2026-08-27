# Resource Manager ImGui View

`CGuiViewResourceManagerImGui` is an opt-in ImGui diagnostics window for `RES_ResourceManager`.

## What It Shows

- Global budget usage (`RES_CacheGetBudgetBytes`, `RES_CacheGetTotalUsedBytes`)
- Cache-only usage (`RES_CacheGetCacheUsedBytes`, entry counts, loading count)
- Per-resource rows from `RES_DebugSnapshotResources(...)`
- Filter by path substring, state, and cache ownership
- Per-row retry button for `RESOURCE_STATE_ERROR`

## Actions

- `Force GC` calls `RES_CacheForceEvictLRU(cacheUsed / 2)`.
- `Clear All Cached` calls `RES_CacheClearAll()` after confirmation.
- `Retry` calls `RES_CacheRetry(path, cacheLinearScaling)`.

## Integration

Instantiate it like any other `CGuiView` and register it with `guiMain`.

```cpp
viewResourceManager = new CGuiViewResourceManagerImGui("Resource Manager", 100, 100, 800, 600);
viewResourceManager->visible = false;
guiMain->AddView(viewResourceManager);
```
