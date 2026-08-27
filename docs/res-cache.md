# RES Image Cache

`RES_Cache*` adds a path-keyed image cache on top of `RES_ResourceManager`.

## When To Use

- Use `RES_CacheGetImage(absolutePath, linearScaling)` for arbitrary filesystem PNG assets that are not pre-registered with `RES_RegisterImage`.
- Use `RES_CachePreload(...)` when you want the cache entry warmed without needing the pointer immediately.
- Keep using `RES_RegisterImage` / `RES_GetImage*` for bundled engine resources and legacy registered assets.

## Identity

- Cache identity is `(absolutePath, linearScaling)`.
- The cache does not normalize paths. Different strings pointing at the same file create different entries.
- Cache entries own a deep-copied `resourcePath`; callers may pass temporary strings.
- Hash hits always verify `resourcePath == absolutePath` before returning the image.

## Behavior

- First fetch creates a persistent `CSlrImage` stub and returns it immediately.
- Repeated fetches for the same `(path, linear)` return the same `CSlrImage *`.
- Different `linearScaling` values return different cache entries.
- Failed loads land in `RESOURCE_STATE_ERROR` and stay there until `RES_CacheRetry(...)` is called.
- Eviction deallocates GPU texture state but keeps the `CSlrImage *` alive, so long-lived callers can hold the pointer safely.

## Eviction

- `RES_CacheForceEvictLRU(bytes)` evicts least-recently-touched cache entries until it frees at least `bytes`, or exhausts loaded cache entries.
- `RES_CacheClearAll()` clears all currently loaded cache entries.
- `RESOURCE_STATE_EVICTING` is a transient claim state used to prevent double-eviction while multiple helpers overlap.
- `LOADING` entries are intentionally skipped by clear/evict helpers.

## Threading Notes

- `CSlrResourceBase::resourceActivatedTime` is atomic.
- `CSlrImage::texturePtr` uses atomic `load/store` semantics through `TexturePtr()` and the internal wrapper.
- `gCurrentResourceMemoryTaken` and `gMaxMemoryForResources` are atomic snapshot counters.
- `RES_DeactivateResource` does not erase from `resourcesByHashcode`; removal is still handled separately by `RES_RemoveResource`.
