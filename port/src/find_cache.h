#ifndef EW2_FIND_CACHE_H
#define EW2_FIND_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cache for UnityEngine.Object.FindObjectsOfType(Type) of a few hot types.
 *
 * Unity 4 answers it by walking its whole object table, ~4 ms per call under emulation, and EW2 asks for
 * HomeUIController every frame (30% of a battle frame). The result is kept in a GC handle and handed out again
 * until something that can change it happens: an object is created (Instantiate/Clone/CreateGameObject/
 * AddComponent), a cached element is destroyed, a level is loaded, or the entry is older than a few frames.
 *
 *   EW2_FIND_CACHE=Type1,Type2   types to cache (default HomeUIController); 0 turns the cache off
 *   EW2_FIND_CACHE_FRAMES=N      longest age of an entry, in frames (default 30)
 *
 * find_cache_wrap() is asked for every icall the engine registers, find_cache_tick() once per frame.
 */
void *find_cache_wrap(const char *name, void *fn);
void find_cache_tick(long frame);

#ifdef __cplusplus
}
#endif

#endif
